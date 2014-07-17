/**
 * $Id: generate.c 1246 2014-06-02 09:07am pdorazio $
 *
 * @brief Red Pitaya simple signal/function generator with pre-defined
 *        signal types.
 *
 * @Author Ales Bardorfer <ales.bardorfer@redpitaya.com>
 *
 * (c) Red Pitaya  http://www.redpitaya.com
 *
 * This part of code is written in C programming language.
 * Please visit http://en.wikipedia.org/wiki/C_(programming_language)
 * for more details on the language used herein.
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "fpga_awg.h"
#include "version.h"

#include <unistd.h>
#include <getopt.h>
#include <sys/param.h>
#include "main_osc.h"
#include "fpga_osc.h"

#include <complex.h>    /* Standart Library of Complex Numbers */
#define M_PI 3.14159265358979323846
/**
 * GENERAL DESCRIPTION:
 *
 * The code below performs a function of a signal generator, which produces
 * a a signal of user-selectable pred-defined Signal shape
 * [Sine, Square, Triangle], Amplitude and Frequency on a selected Channel:
 *
 *
 *                   /-----\
 *   Signal shape -->|     | -->[data]--+-->[FPGA buf 1]--><DAC 1>
 *   Amplitude ----->| AWG |            |
 *   Frequency ----->|     |             -->[FPGA buf 2]--><DAC 2>
 *                   \-----/            ^
 *                                      |
 *   Channel ---------------------------+ 
 *
 *
 * This is achieved by first parsing the four parameters defining the 
 * signal properties from the command line, followed by synthesizing the 
 * signal in data[] buffer @ 125 MHz sample rate within the
 * generate_signal() function, depending on the Signal shape, Amplitude
 * and Frequency parameters. The data[] buffer is then transferred
 * to the specific FPGA buffer, defined by the Channel parameter -
 * within the write_signal_fpga() function.
 * The FPGA logic repeatably sends the data from both FPGA buffers to the
 * corresponding DACs @ 125 MHz, which in turn produces the synthesized
 * signal on Red Pitaya SMA output connectors labeled DAC1 & DAC2.
 *
 * Then it acquires up to 16k samples on both Red Pitaya input
 * channels labeled ADC1 & ADC2.
 * 
 * It utilizes the routines of the Oscilloscope module for:
 *   - Triggered ADC signal acqusition to the FPGA buffers.
 *   - Parameter defined averaging & decimation.
 *   - Data transfer to SW buffers.
 *
 * Although the Oscilloscope routines export many functionalities, this 
 * simple signal acquisition utility only exploits a few of them:
 *   - Synchronization between triggering & data readout.
 *   - Only AUTO triggering is used by default.
 *   - Only decimation is parsed to t_params[8].
 *
 * Please feel free to exploit any other scope functionalities via 
 * parameters defined in t_params.
 *
 */

/** Maximal signal frequency [Hz] */
const double c_max_frequency = 62.5e6;

/** Minimal signal frequency [Hz] */
const double c_min_frequency = 0;

/** Maximal signal amplitude [Vpp] */
const double c_max_amplitude = 2.0;

/** AWG buffer length [samples]*/
#define n (16*1024)

/** AWG data buffer */
int32_t data[n];

/** Program name */
const char *g_argv0 = NULL;

/** Signal types */
typedef enum {
    eSignalSine,         ///< Sinusoidal waveform.
    eSignalSquare,       ///< Square waveform.
    eSignalTriangle,     ///< Triangular waveform.
    eSignalSweep         ///< Sinusoidal frequency sweep.
} signal_e;

/** AWG FPGA parameters */
typedef struct {
    int32_t  offsgain;   ///< AWG offset & gain.
    uint32_t wrap;       ///< AWG buffer wrap value.
    uint32_t step;       ///< AWG step interval.
} awg_param_t;

/* Forward declarations */
void synthesize_signal(double ampl, double freq, signal_e type, double endfreq,
                       int32_t *data,
                       awg_param_t *params);
void write_data_fpga(uint32_t ch,
                     const int32_t *data,
                     const awg_param_t *awg);
/** Oscilloscope module parameters as defined in main module
 * @see rp_main_params
 */
float t_params[PARAMS_NUM] = { 0, 1e6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/** Max decimation index */
#define DEC_MAX 6

/** Decimation translation table [COMENTED BECAUSE NOTU USED AT GENERATOR] */ 
static int g_dec[DEC_MAX] = { 1,  8,  64,  1024,  8192,  65536 };

/*the most used variable in for loops*/
int i, i1, i2, i3;

/** Print usage information */
void usage() {

    const char *format =
        "\n"
        "Usage: %s  frequency amplitude samples <DEC> <parameters> <sig. type> <end frequency>\n"
        "\n"
        "\tfrequency          Signal (start) frequency in Hz [%2.1f - %2.1e].\n"
        "\tamplitude          Peak-to-peak signal amplitude in Vpp [0.0 - %1.1f] only Output 1 will be set recomended = 1.0V\n"
        "\tsamples            Number of samples to acquire [0 - %u ].\n"
        "\tDEC                Decimation [%u,%u,%u,%u,%u,%u] (default: 1).\n"
        //"\tchannel          Channel to generate signal on [1, 2].\n"
        //"\ttype               Signal type [sine, sqr, tri, sweep].\n"
        "\tend frequency      Sweep-to frequency in Hz [%2.1f - %2.1e](set this value to start freq. for measurement_sweep)\n"
        "\tMeasurement sweep  number of mesurements (averaged resoults) [max 10]\n"
        "\tCalibration        set to 1 to initiate calibration. default 0\n"
        "\n";

    fprintf( stderr, format, g_argv0, c_min_frequency, c_max_frequency, c_max_amplitude, SIGNAL_LENGTH,g_dec[0],g_dec[1],g_dec[2],g_dec[3],g_dec[4],g_dec[5],c_min_frequency, c_max_frequency);
}


/** Gain string (lv/hv) to number (0/1) transformation */
int get_gain(int *gain, const char *str)
{
    if ( (strncmp(str, "lv", 2) == 0) || (strncmp(str, "LV", 2) == 0) ) {
        *gain = 0;
        return 0;
    }
    if ( (strncmp(str, "hv", 2) == 0) || (strncmp(str, "HV", 2) == 0) ) {
        *gain = 1;
        return 0;
    }

    fprintf(stderr, "Unknown gain: %s\n", str);
    return -1;
}

float *create_table() {
    float *new_table = (float *)malloc( 22 * sizeof(float));
    return new_table;
}

float *create_table_size(int num_of_el) {
    float *new_table = (float *)malloc( num_of_el * sizeof(float));
    return new_table;
}

float **create_2D_table_size(int num_of_rows, int num_of_cols) {
    float **new_table = (float **)malloc( num_of_rows * sizeof(float*));
        for(i = 0; i < num_of_rows; i++) {
            new_table[i] = create_table_size(num_of_cols);
        }
    return new_table;
}

float max_array(float *arrayptr, int numofelements) {
  int i = 0;
  float max = -100000;//seting the minimum value possible

  for(i = 0; i < numofelements; i++)
  {
    if(max < arrayptr[i])
    {
      max = arrayptr[i];
    }
  }
  return max;
}

float trapz(float *arrayptr, float *dT, int size) {
  float result;
  int i;
  //printf("size = %d\n", size);
  for (i =0; i < size ; i++) {
    result += fabsf((dT[ i+1 ] - dT[ i ]) * ( arrayptr[i] - arrayptr[ i+1 ] )/(float)2);
  }
    return result;
}

float mean_array(float *arrayptr, int numofelements) {
  int i = 1;
  float mean = 0;

  for(i = 0; i < numofelements; i++)
  {
    mean += arrayptr[i];
  }

  mean = mean / numofelements;
  return mean;
}

float mean_array_column(float **arrayptr, int length, int column) {
    float result;
    int i;
    for(i=0; i < length; i++) {
        result += arrayptr[i][column];
    }
    return (result / length);
}

/** lcr main */
int main(int argc, char *argv[])
{

    /*Setting measuring parameters (LCR Pitaya DT)*/
    double Rs = 8200; // Set value of shunt resistor 
    double DC_bias = 0; // Set value od DC volatge on outputs 
    uint32_t averaging_num = 5; // Number of measurments for averaging
    uint32_t min_periodes =15; // max 20
    /* frequency sweep */
    double one_calibration;



    g_argv0 = argv[0];
    int equal = 0;
    int shaping = 0;    

    if ( argc < 3 ) {

        usage();
        exit( EXIT_FAILURE );
    }

    /* Signal frequency argument parsing */
    //double start_frequency = strtod(argv[1], NULL);
    double start_frequency = 1000;
    /* Check frequency limits */
    if ( (start_frequency < c_min_frequency) || (start_frequency > c_max_frequency ) ) {
        fprintf(stderr, "Invalid start frequency: %s\n", argv[1]);
        usage();
        exit( EXIT_FAILURE );
    }
    
    /* Signal amplitude argument parsing */
    //double ampl = strtod(argv[2], NULL);
    double ampl = 2;
    if ( (ampl < 0.0) || (ampl > c_max_amplitude) ) {
        fprintf(stderr, "Invalid amplitude: %s\n", argv[2]);
        usage();
        exit( EXIT_FAILURE );
    }

    /* Acqusition size */
    //uint32_t size = atoi(argv[3]);
    uint32_t size = 16384;
    if (size > SIGNAL_LENGTH) {
            fprintf(stderr, "Invalid SIZE: %s\n", argv[3]);
            usage();
            exit( EXIT_FAILURE );
        }

    /*Decimation*/
    /*uint32_t idx = 1;
    uint32_t dec = atoi(argv[4]);
    for (idx = 0; idx < DEC_MAX; idx++) {
        if (dec == g_dec[idx]) {
            break;
        }
    }
    if (idx != DEC_MAX) {
        t_params[TIME_RANGE_PARAM] = idx;
    } 
    else {
        fprintf(stderr, "Invalid decimation DEC: %s\n", argv[4]);
        usage();
        return -1;
    }
    */



    /* Signal type argument parsing */
    /* LCR meter only uses sine signal for outputs for now; TODO enable other signals */
    signal_e type = eSignalSine;
    /*
    if ( strcmp(argv[4], "sine") == 0) {
            type = eSignalSine;
        } else if ( strcmp(argv[5], "sqr") == 0) {
            type = eSignalSquare;
        } else if ( strcmp(argv[5], "tri") == 0) {
            type = eSignalTriangle;
        } else if ( strcmp(argv[5], "sweep") == 0) {
            type = eSignalSweep;   
        } else {
            fprintf(stderr, "Invalid signal type: %s\n", argv[5]);
            usage();
            return -1;
        }
    */

    /* End frequency */
    //double end_frequency = 0;
    double end_frequency = 10000;
    //end_frequency = strtod(argv[5], NULL);
    if (end_frequency > c_max_frequency) {
        end_frequency = c_max_frequency;
        printf("end frequency set too high. now set to max value (%2.1e)\n",c_max_frequency);
    }

    uint32_t frequency_step = 1000;

    /* Measurement sweep */
    double measurement_sweep = 5;
    //measurement_sweep = strtod(argv[6], NULL);
    if (measurement_sweep > 10) {
        measurement_sweep = 10;
        printf("measurement sweep set too high [MAX = 10], changed to max");
    }

    int calibration = 1;
    //calibration = strtod(argv[7], NULL);
    if(calibration ==1){
        printf("calibration initiated\n");
    }

    /* endfreq set to 0 because sweep is done in anothef foor loop */
    double endfreq = 0;

    /* only chanel 1 is used */
    uint32_t ch = 0;

    /* if user sets the measuring_sweep and end frequency than end frequency will prevail and program will sweep in frequency domain */
    if (end_frequency > start_frequency) {                        
       measurement_sweep = 1;                    
    }

    /*
    * Calibration sequence
    */

    /* the program waits for the user to make a short connection */
    char calibration_continue;
    while(1) {
      printf("Short connection calibration. continue? [y|n] :");
      if (scanf( "%c", &calibration_continue) > 0) 
      {
        if(calibration_continue=='y') 
          {break;}
        else if (calibration_continue=='Y') 
          {break;}
        else if (calibration_continue == 'n') 
          {return 0;}
        else if (calibration_continue == 'N') 
          {return 0;}
        else 
          {return -1;}
      }
      else {
        printf("error when readnig from stdinput (scanf)\n");
        return -1;
      }
    }
    


    /* Memory initialization */
    // derivative tie for trapezoidal function
    int N;
    float T;
    float *dT = create_table();
    dT[0] = 20;
    printf("dt = %f\n",dT[0] );
    //time vector
    float *t  = create_table();
    // Acquired data is stored in s array
    float **s; //= create_2D_table_size( 16384, 3);
    s = (float **)malloc(SIGNALS_NUM * sizeof(float *));
        for(i = 0; i < SIGNALS_NUM; i++) {
            s[i] = (float *)malloc(SIGNAL_LENGTH * sizeof(float));
        }
    // acquired data is converted to volrage and stored in U_acq
    float **U_acq = create_2D_table_size(SIGNALS_NUM, SIGNAL_LENGTH);
    // number of acquired signals
    int sig_num, sig_len;
    // return value from acquire
    int ret_val;
    // number of acquire retries for acquiring the data
    int retries = 150000;
    // acquired signal size
    int signal_size;
    // Used for storing the Voltage and current on the load
    float *U_load = create_table_size( SIGNAL_LENGTH );
    float *I_load = create_table_size( SIGNAL_LENGTH );
    //Signals multiplied by the reference signal (sin)
    float **U_load_ref = create_2D_table_size(SIGNALS_NUM, SIGNAL_LENGTH); //U_load_ref[1][i] voltage signal 1, U_load_ref[2][i] - voltage signal 2
    float **I_load_ref = create_2D_table_size(SIGNALS_NUM, SIGNAL_LENGTH); //I_load_ref[1][i] current signal 1, I_load_ref[2][i] - current signal 2
    //Signals return by trapezoidal method in complex
    float *X_trapz = create_table_size( SIGNALS_NUM );
    float *Y_trapz = create_table_size( SIGNALS_NUM );

    // Voltage and its phase and current and its pahse calculated from lock in method
    float U_load_amp ;
    float Phase_U_load_amp;
    float I_load_amp;
    float Phase_I_load_amp;
    float complex Z;
    float Z_phase_deg_imag;  // may cuse errors because not complex
    //float U_load_max; //comented because not used
    //float I_load_max; // comented because noy used

    /* Data storage calibration */
    float **Calib_data_short_avreage = create_2D_table_size(averaging_num, 4); //appendin 4 data values
    float **Calib_data_short  = create_2D_table_size(averaging_num, 4); //appendin 4 data values
    /* loop for sweeping trough frequencies */
    double frequency;

    t_params[EQUAL_FILT_PARAM] = equal;
    t_params[SHAPE_FILT_PARAM] = shaping;


    awg_param_t params;
        /* Prepare data buffer (calculate from input arguments) */
        synthesize_signal(ampl, start_frequency, type, endfreq, data, &params);

        /* Write the data to the FPGA and set FPGA AWG state machine */
        write_data_fpga(ch, data, &params);



    int equal = 0;
    int shaping = 0;

    /* data acqusition */
    /* Filter parameters */
    t_params[EQUAL_FILT_PARAM] = equal;
    t_params[SHAPE_FILT_PARAM] = shaping;

    /* Initialization of Oscilloscope application */
    if(rp_app_init() < 0) {
        fprintf(stderr, "rp_app_init() failed!\n");
        return -1;
    }

    /* Setting of parameters in Oscilloscope main module */
    if(rp_set_params((float *)&t_params, PARAMS_NUM) < 0) {
        fprintf(stderr, "rp_set_params() failed!\n");
        return -1;
    }

    for ( frequency = start_frequency ; frequency < end_frequency ; frequency += frequency_step) {

        printf("zanka 1 frequency sweep \n");
        double w_out = frequency * 2 * M_PI; //omega 

        


        /* measurement_sweep defines if   */
        if (measurement_sweep > 1) {
            one_calibration = measurement_sweep - 1;  //4 = 5 - 1 
        }
        else {
            one_calibration = 0;    //ce je measurment_sweep = 1 potem postavimo one_calibration = 0 in naredimo vec kaibacij ?
        }
        printf("one_calibration = %f\n",one_calibration );


        for (i = 0; i < (measurement_sweep - one_calibration); i++ ) {  // For measurment sweep is 1. calibration   //s = 1:1:(1-0) 
        
            for ( i1 = 0; i1 < averaging_num; i1++ ) {
                printf("zanka 2 for zanka averaging_num \n");
                /* seting number of acquired samples */
                int f;
                if (frequency >= 160000) {
                    f=0;
                    printf("f = 0\n");
                }
                else if (frequency >= 20000) {
                    f=1;
                    printf("f = 1\n");
                }    
                else if (frequency >= 2500) {
                    f=2;
                    printf("f = 2\n");
                }    
                else if (frequency >= 160) {
                    f=3;
                    printf("f = 3\n");
                }    
                else if (frequency >= 20) {
                    f=4;
                    printf("f = 4\n");
                }     
                else if (frequency >= 2.5) {
                    f=5;
                    printf("f = 5\n");
                }

                //setting decimtion
                t_params[TIME_RANGE_PARAM] = f;

                printf("frequency = %e \n", frequency);
                /*Number of sampels in respect to numbers  of periods T*/
                N = round( ( min_periodes * 125e6 ) / ( frequency * g_dec[f] ) );
                printf("N = %d\n", N );
                /*Sampling time in seconds*/

                T = ( g_dec[f] / 125e6 );
                printf("T = %f\n", T);


                /*time increment*/
                
                for (i2 = 0; i2 < (N - 1); i2++) {
                    dT[i2] = i2 * (float)T;
                }
                /*
                for(i2 = 0; i2 < (N - 1); i2++) {
                    t[i2] = i2;
                }
                */
                /*sending the parameters to pitaya*/
                /* Filter parameters */

                
                size = 16384;
                
                while(retries >= 0) {
                    stevc++;
                    printf("st = %d\n",stevc);
                    //printf("sig_num = %d \n" , sig_num );
                    //printf("sig_len = %d \n" , sig_len );
                    if((ret_val = rp_get_signals(&s, &sig_num, &sig_len)) >= 0) {
                        /* Signals acquired in s[][]:
                         * s[0][i] - TODO
                         * s[1][i] - Channel ADC1 raw signal
                         * s[2][i] - Channel ADC2 raw signal
                         */
                
                        for(i2 = 0; i2 < MIN(size, sig_len); i2++) {
                            //printf("%7d %7d\n", (int)s[1][j], (int)s[2][j]);
                        }
                        break;
                    }

                    if(retries-- == 0) {
                        fprintf(stderr, "Signal scquisition was not triggered!\n");
                        break;
                    }
                    usleep(1000);
                }







                printf("data acquired!\n");
                // acquired signal size
                signal_size = MIN(size, sig_len);

                /* Transform signals from  AD - 14 bit to voltage [ ( s / 2^14 ) * 2 ] */
                for (i2 = 0; i2 < SIGNALS_NUM; i2++) { // only the 1 and 2 are used for i2
                    for(i3=0; i3 < signal_size; i3++ ) { 
                        U_acq[i2][i3] = ( s[i2][i3] * (float)( 2 - DC_bias ) ) / 16384; //division comes after multiplication, this way no accuracy is lost
                    }
                }

                /* Voltage and current on the load can be calculated from gathered data */
                for (i2 = 0; i2 < signal_size; i2++) { 
                    U_load[i2] = U_acq[2][i3] - U_acq[1][i2]; // potencial difference gives the voltage
                    I_load[i2] = U_acq[2][i2] / Rs; // Curent trough the load is the same as trough thr Rs. ohm's law is used to calculate the current
                }

                /* Finding max values, used for ploting */
                /* COMENTED BECAUSE NOT USED
                U_load_max = max_array( U_load , SIGNAL_LENGTH );
                I_load_max = max_array( I_load , SIGNAL_LENGTH );
                */
                /* Acquired signals must be multiplied by the reference signals, used for lock in metod */
                for( i2 = 0; i2 < signal_size; i2++) {
                    U_load_ref[1][i2] = U_load[i2] * sin( t[i2] * T * w_out );
                    U_load_ref[2][i2] = U_load[i2] * cos( t[i2] * T * w_out );
                    I_load_ref[1][i2] = I_load[i2] * sin( t[i2] * T * w_out );
                    I_load_ref[2][i2] = I_load[i2] * cos( t[i2] * T * w_out );
                }

                /* Trapezoidal method for calculating the approximation of an integral */
                X_trapz[1] = trapz( U_load_ref[ 1 ], dT, SIGNAL_LENGTH );
                X_trapz[2] = trapz( U_load_ref[ 2 ], dT, SIGNAL_LENGTH );
                Y_trapz[1] = trapz( U_load_ref[ 1 ], dT, SIGNAL_LENGTH );
                Y_trapz[2] = trapz( U_load_ref[ 2 ], dT, SIGNAL_LENGTH );


                /* Calculating voltage amplitude and phase */
                U_load_amp = sqrtf( pow( X_trapz[1] , (float)2 ) + pow( Y_trapz[1] , (float)2 ));
                Phase_U_load_amp = atan2f( Y_trapz[1], X_trapz[1] );

                /* Calculating current amplitude and phase */
                I_load_amp = sqrtf( pow( X_trapz[2] , (float)2 ) + pow( Y_trapz[2] , (float)2 ));
                Phase_I_load_amp = atan2f( Y_trapz[2], X_trapz[2] );

                /* Asigning impedance  values (complex value) */
                Z = (U_load_amp / I_load_amp) + ( Phase_U_load_amp - Phase_I_load_amp ) * I;
                
                Z_phase_deg_imag = cimag(Z) * (180 / M_PI);
                if ( Z_phase_deg_imag <= -180 ) {
                    Z_phase_deg_imag += 360;
                }
                else if (Z_phase_deg_imag <= 180) {
                    Z_phase_deg_imag -= 360;
                }

            } // for ( i1 = 0; i < averaging_num; i1++ ) {
            
            /* Saving data */
            Calib_data_short_avreage[i1][0] = i1;
            Calib_data_short_avreage[i1][1] = frequency;
            Calib_data_short_avreage[i1][2] = creal(Z);
            Calib_data_short_avreage[i1][3] = cimag(Z);
        } // for (i = 0; i < (measurement_sweep - one_calibration); i++ ) { 
        printf("calculating last parameters...\n");
        Calib_data_short[i][0] = i;
        Calib_data_short[i][1] = frequency;
        Calib_data_short[i][2] = mean_array_column(Calib_data_short_avreage, averaging_num, 2); // mean value of real impedance
        Calib_data_short[i][3] = mean_array_column(Calib_data_short_avreage, averaging_num, 3); // mean value of imaginary impedance
        printf("Calib_data_short[%d][2] = %f\n",i, Calib_data_short[i][2]);       
    } //for ( frequency = start_frequency ; frequency < end_frequency ; frequency += frequency_step) {

    
    return 0;

}

/**
 * Synthesize a desired signal.
 *
 * Generates/synthesized  a signal, based on three pre-defined signal
 * types/shapes, signal amplitude & frequency. The data[] vector of 
 * samples at 125 MHz is generated to be re-played by the FPGA AWG module.
 *
 * @param ampl  Signal amplitude [Vpp].
 * @param freq  Signal frequency [Hz].
 * @param type  Signal type/shape [Sine, Square, Triangle].
 * @param data  Returned synthesized AWG data vector.
 * @param awg   Returned AWG parameters.
 *
 */
void synthesize_signal(double ampl, double freq, signal_e type, double endfreq,
                       int32_t *data,
                       awg_param_t *awg) {

    uint32_t i;

    /* Various locally used constants - HW specific parameters */
    const int dcoffs = -155;
    const int trans0 = 30;
    const int trans1 = 300;
    const double tt2 = 0.249;

    /* This is where frequency is used... */
    awg->offsgain = (dcoffs << 16) + 0x1fff;
    awg->step = round(65536 * freq/c_awg_smpl_freq * n);
    awg->wrap = round(65536 * (n-1));

    int trans = freq / 1e6 * trans1; /* 300 samples at 1 MHz */
    uint32_t amp = ampl * 4000.0;    /* 1 Vpp ==> 4000 DAC counts */
    if (amp > 8191) {
        /* Truncate to max value if needed */
        amp = 8191;
    }

    if (trans <= 10) {
        trans = trans0;
    }


    /* Fill data[] with appropriate buffer samples */
    for(i = 0; i < n; i++) {
        
        /* Sine */
        if (type == eSignalSine) {
            data[i] = round(amp * cos(2*M_PI*(double)i/(double)n));
        }
 
        /* Square */
        if (type == eSignalSquare) {
            data[i] = round(amp * cos(2*M_PI*(double)i/(double)n));
            if (data[i] > 0)
                data[i] = amp;
            else 
                data[i] = -amp;

            /* Soft linear transitions */
            double mm, qq, xx, xm;
            double x1, x2, y1, y2;    

            xx = i;       
            xm = n;
            mm = -2.0*(double)amp/(double)trans; 
            qq = (double)amp * (2 + xm/(2.0*(double)trans));
            
            x1 = xm * tt2;
            x2 = xm * tt2 + (double)trans;
            
            if ( (xx > x1) && (xx <= x2) ) {  
                
                y1 = (double)amp;
                y2 = -(double)amp;
                
                mm = (y2 - y1) / (x2 - x1);
                qq = y1 - mm * x1;

                data[i] = round(mm * xx + qq); 
            }
            
            x1 = xm * 0.75;
            x2 = xm * 0.75 + trans;
            
            if ( (xx > x1) && (xx <= x2)) {  
                    
                y1 = -(double)amp;
                y2 = (double)amp;
                
                mm = (y2 - y1) / (x2 - x1);
                qq = y1 - mm * x1;
                
                data[i] = round(mm * xx + qq); 
            }
        }
        
        /* Triangle */
        if (type == eSignalTriangle) {
            data[i] = round(-1.0*(double)amp*(acos(cos(2*M_PI*(double)i/(double)n))/M_PI*2-1));
        }

        /* Sweep */
        /* Loops from i = 0 to n = 16*1024. Generates a sine wave signal that
           changes in frequency as the buffer is filled. */
        double start = 2 * M_PI * freq;
        double end = 2 * M_PI * endfreq;
        if (type == eSignalSweep) {
            double sampFreq = c_awg_smpl_freq; // 125 MHz
            double t = i / sampFreq; // This particular sample
            double T = n / sampFreq; // Wave period = # samples / sample frequency
            /* Actual formula. Frequency changes from start to end. */
            data[i] = round(amp * (sin((start*T)/log(end/start) * ((exp(t*log(end/start)/T)-1)))));
        }
        
        /* TODO: Remove, not necessary in C/C++. */
        if(data[i] < 0)
            data[i] += (1 << 14);
    }
}

/**
 * Write synthesized data[] to FPGA buffer.
 *
 * @param ch    Channel number [0, 1].
 * @param data  AWG data to write to FPGA.
 * @param awg   AWG paramters to write to FPGA.
 */
void write_data_fpga(uint32_t ch,
                     const int32_t *data,
                     const awg_param_t *awg) {

    uint32_t i;

    fpga_awg_init();

    if(ch == 0) {
        /* Channel A */
        g_awg_reg->state_machine_conf = 0x000041;
        g_awg_reg->cha_scale_off      = awg->offsgain;
        g_awg_reg->cha_count_wrap     = awg->wrap;
        g_awg_reg->cha_count_step     = awg->step;
        g_awg_reg->cha_start_off      = 0;

        for(i = 0; i < n; i++) {
            g_awg_cha_mem[i] = data[i];
        }
    } else {
        /* Channel B */
        g_awg_reg->state_machine_conf = 0x410000;
        g_awg_reg->chb_scale_off      = awg->offsgain;
        g_awg_reg->chb_count_wrap     = awg->wrap;
        g_awg_reg->chb_count_step     = awg->step;
        g_awg_reg->chb_start_off      = 0;

        for(i = 0; i < n; i++) {
            g_awg_chb_mem[i] = data[i];
        }
    }

    /* Enable both channels */
    /* TODO: Should this only happen for the specified channel?
     *       Otherwise, the not-to-be-affected channel is restarted as well
     *       causing unwanted disturbances on that channel.
     */
    g_awg_reg->state_machine_conf = 0x110011;

    fpga_awg_exit();
}