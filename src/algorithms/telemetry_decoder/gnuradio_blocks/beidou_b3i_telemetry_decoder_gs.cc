/*!
 * \file beidou_b3i_telemetry_decoder_gs.cc
 * \brief Implementation of an adapter of a BEIDOU B31 DNAV data decoder block
 * \author Damian Miralles, 2019. dmiralles2009(at)gmail.com
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include "beidou_b3i_telemetry_decoder_gs.h"
#include "Beidou_B3I.h"
#include "Beidou_DNAV.h"
#include "beidou_dnav_almanac.h"
#include "beidou_dnav_ephemeris.h"
#include "beidou_dnav_iono.h"
#include "beidou_dnav_utc_model.h"
#include "display.h"
#include "gnss_synchro.h"
#include <glog/logging.h>
#include <gnuradio/io_signature.h>
#include <matio.h>          // for Mat_VarCreate
#include <pmt/pmt.h>        // for make_any
#include <pmt/pmt_sugar.h>  // for mp
#include <cstdlib>          // for abs
#include <exception>        // for exception
#include <iostream>         // for cout
#include <memory>           // for shared_ptr, make_shared
#include <vector>

#define CRC_ERROR_LIMIT 8

beidou_b3i_telemetry_decoder_gs_sptr
beidou_b3i_make_telemetry_decoder_gs(const Gnss_Satellite &satellite,
    bool dump)
{
    return beidou_b3i_telemetry_decoder_gs_sptr(new beidou_b3i_telemetry_decoder_gs(satellite, dump));
}


beidou_b3i_telemetry_decoder_gs::beidou_b3i_telemetry_decoder_gs(
    const Gnss_Satellite &satellite, bool dump)
    : gr::block("beidou_b3i_telemetry_decoder_gs",
          gr::io_signature::make(1, 1, sizeof(Gnss_Synchro)),
          gr::io_signature::make(1, 1, sizeof(Gnss_Synchro)))
{
    // prevent telemetry symbols accumulation in output buffers
    this->set_max_noutput_items(1);
    // Ephemeris data port out
    this->message_port_register_out(pmt::mp("telemetry"));
    // Control messages to tracking block
    this->message_port_register_out(pmt::mp("telemetry_to_trk"));
    // initialize internal vars
    d_dump = dump;
    d_satellite = Gnss_Satellite(satellite.get_system(), satellite.get_PRN());
    LOG(INFO) << "Initializing BeiDou B3I Telemetry Decoding for satellite " << this->d_satellite;

    d_symbol_duration_ms = BEIDOU_B3I_TELEMETRY_SYMBOLS_PER_BIT * BEIDOU_B3I_CODE_PERIOD_MS;
    d_symbols_per_preamble = BEIDOU_DNAV_PREAMBLE_LENGTH_SYMBOLS;
    d_samples_per_preamble = BEIDOU_DNAV_PREAMBLE_LENGTH_SYMBOLS;
    d_preamble_period_samples = BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS;

    // Setting samples of preamble code
    for (int32_t i = 0; i < d_symbols_per_preamble; i++)
        {
            if (BEIDOU_DNAV_PREAMBLE[i] == '1')
                {
                    d_preamble_samples[i] = 1;
                }
            else
                {
                    d_preamble_samples[i] = -1;
                }
        }

    d_required_symbols = BEIDOU_DNAV_SUBFRAME_SYMBOLS + d_samples_per_preamble;
    d_symbol_history.set_capacity(d_required_symbols);

    d_last_valid_preamble = 0;
    d_sent_tlm_failed_msg = false;
    d_flag_valid_word = false;
    // Generic settings
    d_sample_counter = 0;
    d_stat = 0;
    d_preamble_index = 0;
    d_flag_frame_sync = false;
    d_TOW_at_current_symbol_ms = 0U;
    d_TOW_at_Preamble_ms = 0U;
    Flag_valid_word = false;
    d_CRC_error_counter = 0;
    d_flag_preamble = false;
    d_channel = 0;
    flag_SOW_set = false;
}


beidou_b3i_telemetry_decoder_gs::~beidou_b3i_telemetry_decoder_gs()
{
    DLOG(INFO) << "BeiDou B3I Telemetry decoder block (channel " << d_channel << ") destructor called.";
    if (d_dump_file.is_open() == true)
        {
            try
                {
                    d_dump_file.close();
                }
            catch (const std::exception &ex)
                {
                    LOG(WARNING) << "Exception in destructor closing the dump file " << ex.what();
                }
        }
    if (d_dump)
        {
            try
                {
                    save_matfile();
                }
            catch (const std::exception &ex)
                {
                    LOG(WARNING) << "Error saving the .mat file: " << ex.what();
                }
        }
}


int32_t beidou_b3i_telemetry_decoder_gs::save_matfile() const
{
    std::ifstream::pos_type size;
    const int32_t number_of_double_vars = 2;
    const int32_t number_of_int_vars = 2;
    const int32_t epoch_size_bytes = sizeof(uint64_t) + sizeof(double) * number_of_double_vars +
                                     sizeof(int32_t) * number_of_int_vars;
    std::ifstream dump_file;
    std::string dump_filename_ = d_dump_filename;

    std::cout << "Generating .mat file for " << dump_filename_ << '\n';
    dump_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    try
        {
            dump_file.open(dump_filename_.c_str(), std::ios::binary | std::ios::ate);
        }
    catch (const std::ifstream::failure &e)
        {
            std::cerr << "Problem opening dump file:" << e.what() << '\n';
            return 1;
        }
    // count number of epochs and rewind
    int64_t num_epoch = 0;
    if (dump_file.is_open())
        {
            size = dump_file.tellg();
            num_epoch = static_cast<int64_t>(size) / static_cast<int64_t>(epoch_size_bytes);
            if (num_epoch == 0LL)
                {
                    // empty file, exit
                    return 1;
                }
            dump_file.seekg(0, std::ios::beg);
        }
    else
        {
            return 1;
        }
    auto TOW_at_current_symbol_ms = std::vector<double>(num_epoch);
    auto tracking_sample_counter = std::vector<uint64_t>(num_epoch);
    auto TOW_at_Preamble_ms = std::vector<double>(num_epoch);
    auto nav_symbol = std::vector<int32_t>(num_epoch);
    auto prn = std::vector<int32_t>(num_epoch);

    try
        {
            if (dump_file.is_open())
                {
                    for (int64_t i = 0; i < num_epoch; i++)
                        {
                            dump_file.read(reinterpret_cast<char *>(&TOW_at_current_symbol_ms[i]), sizeof(double));
                            dump_file.read(reinterpret_cast<char *>(&tracking_sample_counter[i]), sizeof(uint64_t));
                            dump_file.read(reinterpret_cast<char *>(&TOW_at_Preamble_ms[i]), sizeof(double));
                            dump_file.read(reinterpret_cast<char *>(&nav_symbol[i]), sizeof(int32_t));
                            dump_file.read(reinterpret_cast<char *>(&prn[i]), sizeof(int32_t));
                        }
                }
            dump_file.close();
        }
    catch (const std::ifstream::failure &e)
        {
            std::cerr << "Problem reading dump file:" << e.what() << '\n';
            return 1;
        }

    // WRITE MAT FILE
    mat_t *matfp;
    matvar_t *matvar;
    std::string filename = dump_filename_;
    filename.erase(filename.length() - 4, 4);
    filename.append(".mat");
    matfp = Mat_CreateVer(filename.c_str(), nullptr, MAT_FT_MAT73);
    if (reinterpret_cast<int64_t *>(matfp) != nullptr)
        {
            std::array<size_t, 2> dims{1, static_cast<size_t>(num_epoch)};
            matvar = Mat_VarCreate("TOW_at_current_symbol_ms", MAT_C_DOUBLE, MAT_T_DOUBLE, 2, dims.data(), TOW_at_current_symbol_ms.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("tracking_sample_counter", MAT_C_UINT64, MAT_T_UINT64, 2, dims.data(), tracking_sample_counter.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("TOW_at_Preamble_ms", MAT_C_DOUBLE, MAT_T_DOUBLE, 2, dims.data(), TOW_at_Preamble_ms.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("nav_symbol", MAT_C_INT32, MAT_T_INT32, 2, dims.data(), nav_symbol.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);

            matvar = Mat_VarCreate("PRN", MAT_C_INT32, MAT_T_INT32, 2, dims.data(), prn.data(), 0);
            Mat_VarWrite(matfp, matvar, MAT_COMPRESSION_ZLIB);  // or MAT_COMPRESSION_NONE
            Mat_VarFree(matvar);
        }
    Mat_Close(matfp);

    return 0;
}


void beidou_b3i_telemetry_decoder_gs::decode_bch15_11_01(const int32_t *bits,
    std::array<int32_t, 15> &decbits)
{
    int32_t bit;
    int32_t err;
    std::array<int32_t, 4> reg{-1, -1, -1, -1};
    const std::array<int32_t, 15> errind{14, 13, 10, 12, 6, 9, 4, 11, 0, 5, 7, 8, 1, 3, 2};

    for (uint32_t i = 0; i < 15; i++)
        {
            decbits[i] = bits[i];
        }

    for (uint32_t i = 0; i < 15; i++)
        {
            bit = reg[3];
            reg[3] = reg[2];
            reg[2] = reg[1];
            reg[1] = reg[0];
            reg[0] = bits[i] * bit;
            reg[1] *= bit;
        }

    for (uint32_t i = 0; i < 4; ++i)
        {
            reg[i] = (reg[i] + 1) / 2;
        }

    err = reg[0] + reg[1] * 2 + reg[2] * 4 + reg[3] * 8;

    if (err > 0 and err < 16)
        {
            decbits[errind[err - 1]] *= -1;
        }
}


void beidou_b3i_telemetry_decoder_gs::decode_word(
    int32_t word_counter,
    const float *enc_word_symbols,
    int32_t *dec_word_symbols)
{
    std::array<int32_t, 30> bitsbch{};
    std::array<int32_t, 15> first_branch{};
    std::array<int32_t, 15> second_branch{};

    if (word_counter == 1)
        {
            for (uint32_t j = 0; j < 30; j++)
                {
                    dec_word_symbols[j] = static_cast<int32_t>(enc_word_symbols[j] > 0) ? (1) : (-1);
                }
        }
    else
        {
            for (uint32_t r = 0; r < 2; r++)
                {
                    for (uint32_t c = 0; c < 15; c++)
                        {
                            bitsbch[r * 15 + c] = static_cast<int32_t>(enc_word_symbols[c * 2 + r] > 0) ? (1) : (-1);
                        }
                }

            decode_bch15_11_01(&bitsbch[0], first_branch);
            decode_bch15_11_01(&bitsbch[15], second_branch);

            for (uint32_t j = 0; j < 11; j++)
                {
                    dec_word_symbols[j] = first_branch[j];
                    dec_word_symbols[j + 11] = second_branch[j];
                }

            for (uint32_t j = 0; j < 4; j++)
                {
                    dec_word_symbols[j + 22] = first_branch[11 + j];
                    dec_word_symbols[j + 26] = second_branch[11 + j];
                }
        }
}


void beidou_b3i_telemetry_decoder_gs::decode_subframe(float *frame_symbols)
{
    // 1. Transform from symbols to bits
    std::string data_bits;
    data_bits.reserve(BEIDOU_DNAV_WORDS_SUBFRAME * BEIDOU_DNAV_WORD_LENGTH_BITS);
    std::array<int32_t, 30> dec_word_bits{};

    // Decode each word in subframe
    for (uint32_t ii = 0; ii < BEIDOU_DNAV_WORDS_SUBFRAME; ii++)
        {
            // decode the word
            decode_word((ii + 1), &frame_symbols[ii * 30], dec_word_bits.data());

            // Save word to string format
            for (uint32_t jj = 0; jj < (BEIDOU_DNAV_WORD_LENGTH_BITS); jj++)
                {
                    data_bits.push_back((dec_word_bits[jj] > 0) ? ('1') : ('0'));
                }
        }

    if (d_satellite.get_PRN() > 0 and d_satellite.get_PRN() < 6)
        {
            d_nav.d2_subframe_decoder(data_bits);
        }
    else
        {
            d_nav.d1_subframe_decoder(data_bits);
        }

    // 3. Check operation executed correctly
    if (d_nav.get_flag_CRC_test() == true)
        {
            DLOG(INFO) << "BeiDou DNAV CRC correct in channel " << d_channel
                       << " from satellite " << d_satellite;
        }
    else
        {
            DLOG(INFO) << "BeiDou DNAV CRC error in channel " << d_channel
                       << " from satellite " << d_satellite;
        }
    // 4. Push the new navigation data to the queues
    if (d_nav.have_new_ephemeris() == true)
        {
            // get object for this SV (mandatory)
            const std::shared_ptr<Beidou_Dnav_Ephemeris> tmp_obj =
                std::make_shared<Beidou_Dnav_Ephemeris>(d_nav.get_ephemeris());
            this->message_port_pub(pmt::mp("telemetry"), pmt::make_any(tmp_obj));
            LOG(INFO) << "BEIDOU DNAV Ephemeris have been received in channel"
                      << d_channel << " from satellite " << d_satellite;
            std::cout << TEXT_YELLOW << "New BEIDOU B3I DNAV message received in channel " << d_channel
                      << ": ephemeris from satellite " << d_satellite << TEXT_RESET << '\n';
        }
    if (d_nav.have_new_utc_model() == true)
        {
            // get object for this SV (mandatory)
            const std::shared_ptr<Beidou_Dnav_Utc_Model> tmp_obj =
                std::make_shared<Beidou_Dnav_Utc_Model>(d_nav.get_utc_model());
            this->message_port_pub(pmt::mp("telemetry"), pmt::make_any(tmp_obj));
            LOG(INFO) << "BEIDOU DNAV UTC Model data have been received in channel"
                      << d_channel << " from satellite " << d_satellite;
            std::cout << TEXT_YELLOW << "New BEIDOU B3I DNAV utc model message received in channel "
                      << d_channel << ": UTC model parameters from satellite "
                      << d_satellite << TEXT_RESET << '\n';
        }
    if (d_nav.have_new_iono() == true)
        {
            // get object for this SV (mandatory)
            const std::shared_ptr<Beidou_Dnav_Iono> tmp_obj =
                std::make_shared<Beidou_Dnav_Iono>(d_nav.get_iono());
            this->message_port_pub(pmt::mp("telemetry"), pmt::make_any(tmp_obj));
            LOG(INFO) << "BEIDOU DNAV Iono data have been received in channel" << d_channel
                      << " from satellite " << d_satellite;
            std::cout << TEXT_YELLOW << "New BEIDOU B3I DNAV Iono message received in channel "
                      << d_channel << ": Iono model parameters from satellite "
                      << d_satellite << TEXT_RESET << '\n';
        }
    if (d_nav.have_new_almanac() == true)
        {
            //            unsigned int slot_nbr = d_nav.i_alm_satellite_PRN;
            //            std::shared_ptr<Beidou_Dnav_Almanac> tmp_obj =
            //            std::make_shared<Beidou_Dnav_Almanac>(d_nav.get_almanac(slot_nbr));
            //            this->message_port_pub(pmt::mp("telemetry"),
            //            pmt::make_any(tmp_obj));
            LOG(INFO) << "BEIDOU DNAV Almanac data have been received in channel"
                      << d_channel << " from satellite " << d_satellite << '\n';
            std::cout << TEXT_YELLOW << "New BEIDOU B3I DNAV almanac received in channel " << d_channel
                      << " from satellite " << d_satellite << TEXT_RESET << '\n';
        }
}


void beidou_b3i_telemetry_decoder_gs::set_satellite(
    const Gnss_Satellite &satellite)
{
    uint32_t sat_prn = 0;
    d_satellite = Gnss_Satellite(satellite.get_system(), satellite.get_PRN());
    DLOG(INFO) << "Setting decoder Finite State Machine to satellite "
               << d_satellite;
    DLOG(INFO) << "Navigation Satellite set to " << d_satellite;

    // Update satellite information for DNAV decoder
    sat_prn = d_satellite.get_PRN();
    d_nav.set_satellite_PRN(sat_prn);
    d_nav.set_signal_type(5);  // BDS: data source (0:unknown,1:B1I,2:B1Q,3:B2I,4:B2Q,5:B3I,6:B3Q)

    // Update tel dec parameters for D2 NAV Messages
    if (sat_prn > 0 and sat_prn < 6)
        {
            d_symbols_per_preamble = BEIDOU_DNAV_PREAMBLE_LENGTH_SYMBOLS;
            d_samples_per_preamble = BEIDOU_DNAV_PREAMBLE_LENGTH_SYMBOLS;
            d_preamble_period_samples = BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS;

            // Setting samples of preamble code
            for (int32_t i = 0; i < d_symbols_per_preamble; i++)
                {
                    if (BEIDOU_DNAV_PREAMBLE[i] == '1')
                        {
                            d_preamble_samples[i] = 1;
                        }
                    else
                        {
                            d_preamble_samples[i] = -1;
                        }
                }
            d_symbol_duration_ms = BEIDOU_B3I_GEO_TELEMETRY_SYMBOLS_PER_BIT * BEIDOU_B3I_CODE_PERIOD_MS;
            d_required_symbols = BEIDOU_DNAV_SUBFRAME_SYMBOLS + d_samples_per_preamble;
            d_symbol_history.set_capacity(d_required_symbols);
        }
    else
        {
            // back to normal satellites
            d_symbol_duration_ms = BEIDOU_B3I_TELEMETRY_SYMBOLS_PER_BIT * BEIDOU_B3I_CODE_PERIOD_MS;
            d_symbols_per_preamble = BEIDOU_DNAV_PREAMBLE_LENGTH_SYMBOLS;
            d_samples_per_preamble = BEIDOU_DNAV_PREAMBLE_LENGTH_SYMBOLS;
            d_preamble_period_samples = BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS;

            // Setting samples of preamble code
            for (int32_t i = 0; i < d_symbols_per_preamble; i++)
                {
                    if (BEIDOU_DNAV_PREAMBLE[i] == '1')
                        {
                            d_preamble_samples[i] = 1;
                        }
                    else
                        {
                            d_preamble_samples[i] = -1;
                        }
                }

            d_required_symbols = BEIDOU_DNAV_SUBFRAME_SYMBOLS + d_samples_per_preamble;
            d_symbol_history.set_capacity(d_required_symbols);
        }
}


void beidou_b3i_telemetry_decoder_gs::set_channel(int32_t channel)
{
    d_channel = channel;
    LOG(INFO) << "Navigation channel set to " << channel;
    // ############# ENABLE DATA FILE LOG #################
    if (d_dump == true)
        {
            if (d_dump_file.is_open() == false)
                {
                    try
                        {
                            d_dump_filename = "telemetry";
                            d_dump_filename.append(std::to_string(d_channel));
                            d_dump_filename.append(".dat");
                            d_dump_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
                            d_dump_file.open(d_dump_filename.c_str(),
                                std::ios::out | std::ios::binary);
                            LOG(INFO) << "Telemetry decoder dump enabled on channel " << d_channel
                                      << " Log file: " << d_dump_filename.c_str();
                        }
                    catch (const std::ifstream::failure &e)
                        {
                            LOG(WARNING) << "channel " << d_channel
                                         << ": exception opening Beidou TLM dump file. "
                                         << e.what();
                        }
                }
        }
}


void beidou_b3i_telemetry_decoder_gs::reset()
{
    d_last_valid_preamble = d_sample_counter;
    d_TOW_at_current_symbol_ms = 0;
    d_sent_tlm_failed_msg = false;
    d_flag_valid_word = false;
    DLOG(INFO) << "Beidou B3I Telemetry decoder reset for satellite " << d_satellite;
}


int beidou_b3i_telemetry_decoder_gs::general_work(
    int noutput_items __attribute__((unused)),
    gr_vector_int &ninput_items __attribute__((unused)),
    gr_vector_const_void_star &input_items, gr_vector_void_star &output_items)
{
    int32_t corr_value = 0;
    int32_t preamble_diff = 0;

    auto **out = reinterpret_cast<Gnss_Synchro **>(&output_items[0]);            // Get the output buffer pointer
    const auto **in = reinterpret_cast<const Gnss_Synchro **>(&input_items[0]);  // Get the input buffer pointer

    Gnss_Synchro current_symbol{};  // structure to save the synchronization
                                    // information and send the output object to the
                                    // next block
    // 1. Copy the current tracking output
    current_symbol = in[0][0];
    d_symbol_history.push_back(current_symbol.Prompt_I);  // add new symbol to the symbol queue
    d_sample_counter++;                                   // count for the processed samples
    consume_each(1);
    d_flag_preamble = false;

    if (d_symbol_history.size() >= d_required_symbols)
        {
            // ******* preamble correlation ********
            for (int32_t i = 0; i < d_samples_per_preamble; i++)
                {
                    if (d_symbol_history[i] < 0)  // symbols clipping
                        {
                            corr_value -= d_preamble_samples[i];
                        }
                    else
                        {
                            corr_value += d_preamble_samples[i];
                        }
                }
        }
    // ******* frame sync ******************
    if (d_stat == 0)  // no preamble information
        {
            if (abs(corr_value) >= d_samples_per_preamble)
                {
                    // Record the preamble sample stamp
                    d_preamble_index = d_sample_counter;
                    DLOG(INFO) << "Preamble detection for BEIDOU B3I SAT " << this->d_satellite;
                    // Enter into frame pre-detection status
                    d_stat = 1;
                }
        }
    else if (d_stat == 1)  // possible preamble lock
        {
            if (abs(corr_value) >= d_samples_per_preamble)
                {
                    // check preamble separation
                    preamble_diff = static_cast<int32_t>(d_sample_counter - d_preamble_index);
                    if (abs(preamble_diff - d_preamble_period_samples) == 0)
                        {
                            // try to decode frame
                            DLOG(INFO) << "Starting BeiDou DNAV frame decoding for BeiDou B3I SAT "
                                       << this->d_satellite;
                            d_preamble_index = d_sample_counter;  // record the preamble sample stamp
                            d_stat = 2;

                            // ******* SAMPLES TO SYMBOLS *******
                            if (corr_value > 0)  // normal PLL lock
                                {
                                    for (uint32_t i = 0; i < BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS; i++)
                                        {
                                            d_subframe_symbols[i] = d_symbol_history[i];
                                        }
                                }
                            else  // 180 deg. inverted carrier phase PLL lock
                                {
                                    for (uint32_t i = 0; i < BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS; i++)
                                        {
                                            d_subframe_symbols[i] = -d_symbol_history[i];
                                        }
                                }

                            // call the decoder
                            decode_subframe(d_subframe_symbols.data());

                            if (d_nav.get_flag_CRC_test() == true)
                                {
                                    d_CRC_error_counter = 0;
                                    d_flag_preamble = true;               // valid preamble indicator (initialized to false every work())
                                    d_preamble_index = d_sample_counter;  // record the preamble sample stamp (t_P)
                                    if (!d_flag_frame_sync)
                                        {
                                            d_flag_frame_sync = true;
                                            DLOG(INFO) << "BeiDou DNAV frame sync found for SAT "
                                                       << this->d_satellite;
                                        }
                                }
                            else
                                {
                                    d_CRC_error_counter++;
                                    d_preamble_index = d_sample_counter;  // record the preamble sample stamp
                                    if (d_CRC_error_counter > CRC_ERROR_LIMIT)
                                        {
                                            DLOG(INFO) << "BeiDou DNAV frame sync lost for SAT "
                                                       << this->d_satellite;
                                            d_flag_frame_sync = false;
                                            d_stat = 0;
                                            flag_SOW_set = false;
                                        }
                                }
                        }
                    else
                        {
                            if (preamble_diff > d_preamble_period_samples)
                                {
                                    d_stat = 0;  // start again
                                }
                            DLOG(INFO) << "Failed BeiDou DNAV frame decoding for BeiDou B3I SAT "
                                       << this->d_satellite;
                        }
                }
        }
    else if (d_stat == 2)  // preamble acquired
        {
            if (d_sample_counter == d_preamble_index + static_cast<uint64_t>(d_preamble_period_samples))
                {
                    // ******* SAMPLES TO SYMBOLS *******
                    if (corr_value > 0)  // normal PLL lock
                        {
                            for (uint32_t i = 0; i < BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS; i++)
                                {
                                    d_subframe_symbols[i] = d_symbol_history[i];
                                }
                        }
                    else  // 180 deg. inverted carrier phase PLL lock
                        {
                            for (uint32_t i = 0; i < BEIDOU_DNAV_PREAMBLE_PERIOD_SYMBOLS; i++)
                                {
                                    d_subframe_symbols[i] = -d_symbol_history[i];
                                }
                        }

                    // call the decoder
                    decode_subframe(d_subframe_symbols.data());

                    if (d_nav.get_flag_CRC_test() == true)
                        {
                            d_CRC_error_counter = 0;
                            d_flag_preamble = true;               // valid preamble indicator (initialized to false every work())
                            d_preamble_index = d_sample_counter;  // record the preamble sample stamp (t_P)
                            if (!d_flag_frame_sync)
                                {
                                    d_flag_frame_sync = true;
                                    DLOG(INFO) << "BeiDou DNAV frame sync found for SAT "
                                               << this->d_satellite;
                                }
                        }
                    else
                        {
                            d_CRC_error_counter++;
                            d_preamble_index = d_sample_counter;  // record the preamble sample stamp
                            if (d_CRC_error_counter > CRC_ERROR_LIMIT)
                                {
                                    DLOG(INFO) << "BeiDou DNAV frame sync lost for SAT "
                                               << this->d_satellite;
                                    d_flag_frame_sync = false;
                                    d_stat = 0;
                                    flag_SOW_set = false;
                                }
                        }
                }
        }
    // UPDATE GNSS SYNCHRO DATA
    // 2. Add the telemetry decoder information
    if (this->d_flag_preamble == true and d_nav.get_flag_new_SOW_available() == true)
        // update TOW at the preamble instant
        {
            // Reporting sow as gps time of week
            d_TOW_at_Preamble_ms = static_cast<uint32_t>((d_nav.get_SOW() + BEIDOU_DNAV_BDT2GPST_LEAP_SEC_OFFSET) * 1000.0);
            // check TOW update consistency
            const uint32_t last_d_TOW_at_current_symbol_ms = d_TOW_at_current_symbol_ms;
            // compute new TOW
            d_TOW_at_current_symbol_ms = d_TOW_at_Preamble_ms + d_required_symbols * d_symbol_duration_ms;
            flag_SOW_set = true;
            d_nav.set_flag_new_SOW_available(false);

            if (last_d_TOW_at_current_symbol_ms != 0 and abs(static_cast<int64_t>(d_TOW_at_current_symbol_ms) - int64_t(last_d_TOW_at_current_symbol_ms)) > static_cast<int64_t>(d_symbol_duration_ms))
                {
                    LOG(INFO) << "Warning: BEIDOU B3I TOW update in ch " << d_channel
                              << " does not match the TLM TOW counter " << static_cast<int64_t>(d_TOW_at_current_symbol_ms) - int64_t(last_d_TOW_at_current_symbol_ms) << " ms \n";

                    d_TOW_at_current_symbol_ms = 0;
                    d_flag_valid_word = false;
                }
            else
                {
                    d_last_valid_preamble = d_sample_counter;
                    d_flag_valid_word = true;
                }
        }
    else
        {
            if (d_flag_valid_word)
                {
                    d_TOW_at_current_symbol_ms += d_symbol_duration_ms;
                    if (current_symbol.Flag_valid_symbol_output == false)
                        {
                            d_flag_valid_word = false;
                        }
                }
        }

    if (d_flag_valid_word == true)
        {
            current_symbol.TOW_at_current_symbol_ms = d_TOW_at_current_symbol_ms;
            current_symbol.Flag_valid_word = d_flag_valid_word;

            if (d_dump == true)
                {
                    // MULTIPLEXED FILE RECORDING - Record results to file
                    try
                        {
                            double tmp_double;
                            uint64_t tmp_ulong_int;
                            int32_t tmp_int;
                            tmp_double = static_cast<double>(d_TOW_at_current_symbol_ms) / 1000.0;
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_double), sizeof(double));
                            tmp_ulong_int = current_symbol.Tracking_sample_counter;
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_ulong_int), sizeof(uint64_t));
                            tmp_double = static_cast<double>(d_TOW_at_Preamble_ms) / 1000.0;
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_double), sizeof(double));
                            tmp_int = (current_symbol.Prompt_I > 0.0 ? 1 : -1);
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_int), sizeof(int32_t));
                            tmp_int = static_cast<int32_t>(current_symbol.PRN);
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_int), sizeof(int32_t));
                        }
                    catch (const std::ifstream::failure &e)
                        {
                            LOG(WARNING) << "Exception writing Telemetry GPS L5 dump file " << e.what();
                        }
                }

            // 3. Make the output (copy the object contents to the GNURadio reserved memory)
            *out[0] = current_symbol;
            return 1;
        }
    return 0;
}
