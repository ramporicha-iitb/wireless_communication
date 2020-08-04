#include <cassert>
#include <itpp/itcomm.h>

using namespace std;
using namespace itpp;

#define DEBUG 1

#if DEBUG
#define DEBUG_MSG(str) do { std::cout << str << std::endl; } while( false )
#else
#define DEBUG_MSG(str) do { } while ( false )
#endif

#define BATCH_SIZE          1e3
#define DEBUG_SYM_COUNT     4

#define CP_LEN              16
#define NUM_SUBCARRIER      128
#define NUM_OFDM_FRAMES     1e5

#define NUM_TX_ANTENNA      2
#define NUM_RX_ANTENNA      2

#define OUTPUT_FILENAME     "mimo_ofdm_tdl.it"

int main(int argc, char *argv[])
{
    RNG_randomize();

    int batch_size;
    int Nsub, Ncp, Nframe;

    Ncp = CP_LEN;
    Nsub = NUM_SUBCARRIER;
    Nframe = NUM_OFDM_FRAMES;
    batch_size = BATCH_SIZE;
    
    assert(Nframe % batch_size == 0);

    // MIMO model 
    int Nt, Nr;
    Nt = NUM_TX_ANTENNA;
    Nr = NUM_RX_ANTENNA;

    assert(Nr >= Nt);

    // Modulators
    OFDM ofdm;
    ofdm.set_parameters(Nsub, Ncp);
    BPSK_c modulator;
    DEBUG_MSG("Symbols: " << modulator.get_symbols());

    // Channels
    AWGN_Channel awgn_channel;

    double Es, Eb;
    Es = 1.0;                                   // Energy per symbol
    Eb = Es / modulator.bits_per_symbol();

    vec EbN0dB = "0:15";                        // Eb/N0 values in dB
    vec N0 = Eb * pow(inv_dB(EbN0dB), -1.0);    // N0 : variance of the complex valued noise

    // Bit error rates
    vec ber;
    BERC berc;
    ber.set_size(EbN0dB.length());

    // Channel coefficient row numbers corresponding to the begining of the OFDM frame
    ivec channel_rows = "0:" + to_string(Nsub + Ncp) + ":" + to_string(batch_size * (Nsub + Ncp) - 1);

    Real_Timer timer;
    timer.tic();
   
    for (int snr_loop = 0; snr_loop < EbN0dB.length(); ++snr_loop)
    {
        size_t err_count = 0;
        size_t total_bit_count = 0;

        TDL_Channel *tdl_channel[Nr][Nt]; // Nr x Nt independent channels
        for (int i = 0; i < Nr; ++i) {
            for (int j = 0; j < Nt; ++j) {
                tdl_channel[i][j] = new TDL_Channel(vec("0 -3 -6 -9"), ivec("0 1 2 3"));
                tdl_channel[i][j]->set_norm_doppler(1e-6);
            }
        }

        for (int frame_loop = 0; frame_loop < Nframe / batch_size; ++frame_loop)
        {
            /*-------------------------- TRANSMITTER -----------------------------*/
            // Generate a vector of random bits to transmit

            // row[i]: data transmitted on i-th tx antenna
            int num_tx_bits = batch_size * Nsub * modulator.bits_per_symbol();
            bmat tx_bits = randb(Nt, num_tx_bits);
            DEBUG_MSG("Tx bits size: " << tx_bits.rows() << "x" << tx_bits.cols());

            // Modulate the bits to symbols
            cmat tx_symbols(Nt, batch_size * Nsub);
            for (int i = 0; i < Nt; ++i) 
            {
                tx_symbols.set_row(i, modulator.modulate_bits(tx_bits.get_row(i)));
            }
            tx_symbols *= 1.0/sqrt(Nt);   // Normalized power
            DEBUG_MSG("Tx symbols size: " << tx_symbols.rows() << "x" << tx_symbols.cols());

            // OFDM modulation
            cmat tx_ofdm_symbols(Nt, batch_size * (Nsub + Ncp));
            for (int i = 0; i < Nt; ++i)
            {
                tx_ofdm_symbols.set_row(i, ofdm.modulate(tx_symbols.get_row(i)));
            }
            DEBUG_MSG("Tx OFDM symbols size: " << tx_ofdm_symbols.rows() << "x" << tx_ofdm_symbols.cols());

            /*----------------------- TDL + AWGN CHANNEL -------------------------*/
            // Generate channel filter coefficients
            // Returns a matrix with one tap per column for each independent channel

            // tdl_channel_coeff[i][j]: 
            // Channel coefficient matrix between i-th rx antenna and j-th tx antenna
            cmat tdl_channel_coeff[Nr][Nt];
            for (int i = 0; i < Nr; ++i) 
            {
                for (int j = 0; j < Nt; ++j) 
                {
                    tdl_channel[i][j]->generate(batch_size * (Nsub + Ncp), tdl_channel_coeff[i][j]);
                }
            }
            DEBUG_MSG("tdl_channel_coeff[0][0] size: " << 
                    tdl_channel_coeff[0][0].rows() << "x" << tdl_channel_coeff[0][0].cols());

            // Run the transmited symbols through the channel
            cvec channel_output_vec;
            cmat tdl_channel_output(Nr, batch_size * (Nsub + Ncp)); // Channel output at each Rx antenna
            tdl_channel_output.clear();

            for (int i = 0; i < Nr; ++i)
            {
                for (int j = 0; j < Nt; ++j)
                {
                    tdl_channel[i][j]->filter_known_channel (
                            tx_ofdm_symbols.get_row(j), channel_output_vec, tdl_channel_coeff[i][j]);

                    // Channel output at i-th Rx antenna = 
                    // Sum total of channel outputs due to each Tx antenna at i-th Rx antenna
                    tdl_channel_output.set_row(i, tdl_channel_output.get_row(i) + 
                            channel_output_vec.left(batch_size * (Nsub + Ncp)));
                }
            }
            DEBUG_MSG("tdl_channel_output size: " << 
                    tdl_channel_output.rows() << "x" << tdl_channel_output.cols());

            // Set the noise variance of the AWGN channel
            awgn_channel.set_noise(N0(snr_loop));

            cmat rx_ofdm_symbols(Nr, batch_size * (Nsub + Ncp));
            for (int i = 0; i < Nr; ++i)
            {
                rx_ofdm_symbols.set_row(i, awgn_channel(tdl_channel_output.get_row(i)));
            }
            DEBUG_MSG("Rx OFDM symbols size: " << rx_ofdm_symbols.rows() << "x" << rx_ofdm_symbols.cols());

            /*------------------------- RECEIVER --------------------------------*/
            // OFDM demodulation
            cmat rx_symbols(Nr, batch_size * Nsub);
            for (int i = 0; i < Nr; ++i)
            {
                rx_symbols.set_row(i, ofdm.demodulate(rx_ofdm_symbols.get_row(i)));
            }
            DEBUG_MSG("Rx symbols size: " << rx_symbols.rows() << "x" << rx_symbols.cols());

            // Zero-Force equalization
            
            // Get channel coefficients at the beginning of the OFDM frame for each independent channel
            // relevant_channel_coeff[i][j], k-th row: 
            // Channel coefficient vector at the begining of k-th OFDM frame
            
            cmat hfft[Nr][Nt];
            cmat relevant_channel_coeff[Nr][Nt];
            for (int i = 0; i < Nr; ++i)
            {
                for (int j = 0; j < Nt; ++j)
                {
                    relevant_channel_coeff[i][j] = tdl_channel_coeff[i][j].get_rows(channel_rows);
                
                    // FFT of channel coefficient vector at the begining of k-th OFDM frame
                    int n_rows = relevant_channel_coeff[i][j].rows();
                    for (int k = 0; k < n_rows; ++k) 
                    {
                        hfft[i][j].append_row(fft(relevant_channel_coeff[i][j].get_row(k), Nsub));
                    }
                }
            }

            /* 2x3 MIMO
             * Solve:
             *  _             _   _     _     _     _
             * |               | |       |   |       |
             * | h11[n] h12[n] | | d1[n] |   | y1[n] |
             * | h21[n] h22[n] | | d2[n] | = | y2[n] |
             * | h31[n] h32[n] | |       |   | y3[n] |
             * |_             _| |_     _|   |_     _|
            */
    
            cvec y(Nr);
            cmat h(Nr,Nt);  // Channel matrix at each time index
            cmat htrans, hinv;
            bmat rx_bits(Nt, batch_size*Nsub);
        
            for (int k = 0; k < batch_size*Nsub; ++k) 
            {
                for (int i = 0; i < Nr; ++i) 
                {
                    for (int j = 0; j < Nt; ++j) 
                    {
                        h(i,j) = hfft[i][j](k / Nsub, k % Nsub);   
                    }
                    y = rx_symbols.get_col(k);
                } 

                if (Nr == Nt) 
                {
                    hinv = inv(h);
                }
                else if (Nr > Nt) {
                    htrans = hermitian_transpose(h);
                    hinv = inv(htrans*h)*htrans;
                }
                rx_bits.set_col(k, modulator.demodulate_bits(hinv*y));
            }
            DEBUG_MSG("Rx bits size: " << rx_bits.rows() << "x" << rx_bits.cols());

            // Calculate the bit error rate
            berc.clear();
            berc.count(rvectorize(tx_bits), rvectorize(rx_bits));
            err_count += berc.get_errors();
            total_bit_count += berc.get_total_bits();
            DEBUG_MSG("Error count: " << err_count << "\t\t" << "Total count: " << total_bit_count);
        }

        ber(snr_loop) = (double)err_count / total_bit_count;
        cout << "Eb/N0 (dB): " << EbN0dB[snr_loop] << "\t\t" << "BER: " << ber(snr_loop) << endl;
        DEBUG_MSG("------------------------------------------------------------------------------"); 
        
        // Free dynamic memory
        for (int i = 0; i < Nr; ++i) {
            for (int j = 0; j < Nt; ++j) {
                delete tdl_channel[i][j];
            }
        }
    }
    DEBUG_MSG("Elapsed time: " << timer.toc());

    // Print the results
    DEBUG_MSG("Eb/N0 (dB): " << EbN0dB);
    DEBUG_MSG("BER: " << ber);

    // Save the results to file
    cout << "Saving results to " << OUTPUT_FILENAME << endl;
    it_file fp;
    fp.open(OUTPUT_FILENAME);
    fp << Name("ebno_dB") << EbN0dB;
    fp << Name("ber") << ber;
    fp.close();

    return 0;
}
