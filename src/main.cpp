#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#define SAMPLE_RATE 1000000.0

#define RX_FREQ_SDR1 890000000.0
#define TX_FREQ_SDR1 800000000.0

#define RX_FREQ_SDR2 800000000.0
#define TX_FREQ_SDR2 890000000.0

#define RX_GAIN 50.0
#define TX_GAIN 50.0

#define RX_ITERATION_COUNT 10
#define TIMEOUT_US 400000

#define NUM_BITS 16
#define SAMPLES_PER_SYMBOL 10
#define AMPLITUDE 20000

static volatile sig_atomic_t running = 1;

static void stop_program(int sig)
{
    (void)sig;
    running = 0;
}

typedef struct {
    SoapySDRDevice *sdr;
    SoapySDRStream *txStream;
    size_t tx_mtu;
    int16_t *tx_buff;
} TxThreadArgs;

void* tx_thread_func(void* arg) {
    TxThreadArgs* args = (TxThreadArgs*)arg;
    size_t buffers_written = 0;
    const long timeoutUs = TIMEOUT_US;

    uint8_t bits[NUM_BITS] = {1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0};
    
    printf("\nGenerated bits: ");
    for (size_t i = 0; i < NUM_BITS; i++) {
        printf("%d ", bits[i]);
    }
    printf("\n\n");

    size_t num_symbols = args->tx_mtu / SAMPLES_PER_SYMBOL;
    int16_t *upsampled_i = (int16_t *)calloc(args->tx_mtu, sizeof(int16_t));
    int16_t *upsampled_q = (int16_t *)calloc(args->tx_mtu, sizeof(int16_t));

    for (size_t s = 0; s < num_symbols; s++) {
        uint8_t bit = bits[s % NUM_BITS];
        int16_t i_val = (bit == 1) ? AMPLITUDE : -AMPLITUDE;
        
        upsampled_i[s * SAMPLES_PER_SYMBOL] = i_val;
        upsampled_q[s * SAMPLES_PER_SYMBOL] = 0;
    }

    int16_t filter_taps[SAMPLES_PER_SYMBOL];
    for (int i = 0; i < SAMPLES_PER_SYMBOL; i++) {
        filter_taps[i] = 1;
    }

    for (size_t n = 0; n < args->tx_mtu; n++) {
        int64_t conv_i = 0;
        int64_t conv_q = 0;

        for (int k = 0; k < SAMPLES_PER_SYMBOL; k++) {
            if (n >= (size_t)k) {
                conv_i += upsampled_i[n - k] * filter_taps[k];
                conv_q += upsampled_q[n - k] * filter_taps[k];
            }
        }
        
        args->tx_buff[n * 2]     = (int16_t)conv_i;
        args->tx_buff[n * 2 + 1] = (int16_t)conv_q;
    }

    free(upsampled_i);
    free(upsampled_q);

    int16_t *zero_buff = (int16_t *)calloc(2 * args->tx_mtu, sizeof(int16_t));

    while (running) {
        int16_t *active_buff = (buffers_written % 2 == 0) ? args->tx_buff : zero_buff;

        size_t samples_sent = 0;
        while (samples_sent < args->tx_mtu && running) {
            void *tx_buffs[] = {active_buff + (samples_sent * 2)};
            int tx_flags = 0; 
            int st = SoapySDRDevice_writeStream(
                args->sdr, args->txStream,
                (const void * const *)tx_buffs,
                args->tx_mtu - samples_sent,
                &tx_flags, 0, timeoutUs);

            if (st < 0) {
                if (st == SOAPY_SDR_TIMEOUT) continue;
                fprintf(stderr, "TX Failed: %d\n", st);
                running = 0;
                break;
            }
            samples_sent += (size_t)st;
        }
        buffers_written++;
    }
    
    free(zero_buff);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 4) return EXIT_FAILURE;

    const char *sdr_id = argv[1];
    const char *mode   = argv[2];
    const char *uri    = argv[3];

    bool sdr1    = (strcmp(sdr_id, "sdr1") == 0);
    bool sdr2    = (strcmp(sdr_id, "sdr2") == 0);
    bool rx_mode = (strcmp(mode, "rx") == 0);
    bool tx_mode = (strcmp(mode, "tx") == 0);
    if ((!sdr1 && !sdr2) || (!rx_mode && !tx_mode)) return EXIT_FAILURE;

    double rx_freq = sdr1 ? RX_FREQ_SDR1 : RX_FREQ_SDR2;
    double tx_freq = sdr1 ? TX_FREQ_SDR1 : TX_FREQ_SDR2;

    printf("Устройство: %s\nРежим: %s\nRX: %.3f МГц\n",
           sdr_id, rx_mode ? "RX" : "TX", rx_freq / 1e6);
    if (tx_mode) printf("TX: %.3f МГц\n", tx_freq / 1e6);

    SoapySDRDevice *sdr = NULL;
    SoapySDRStream *rxStream = NULL, *txStream = NULL;
    int16_t *rx_buffer = NULL, *tx_buff = NULL;
    FILE *file = NULL;
    size_t rx_mtu = 0, tx_mtu = 0;
    size_t channels[] = {0};
    size_t channel_count = sizeof(channels) / sizeof(channels[0]);
    const long timeoutUs = TIMEOUT_US;
    pthread_t tx_thread;
    TxThreadArgs tx_args;
    int exit_code = EXIT_FAILURE;

    size_t total_samples_read = 0;
    size_t target_samples = 0;
    size_t last_block_printed = 0;

    SoapySDRKwargs args = {};
    SoapySDRKwargs_set(&args, "driver", "plutosdr");
    SoapySDRKwargs_set(&args, "uri", uri);
    SoapySDRKwargs_set(&args, "direct", "1");
    SoapySDRKwargs_set(&args, "loopback", "0");
    sdr = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);
    if (!sdr) { fprintf(stderr, "SDR open: %s\n", SoapySDRDevice_lastError()); return EXIT_FAILURE; }
    printf("Устройство успешно открыто\n");

    SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, SAMPLE_RATE);
    SoapySDRDevice_setFrequency (sdr, SOAPY_SDR_RX, 0, rx_freq, NULL);
    SoapySDRDevice_setGainMode  (sdr, SOAPY_SDR_RX, *channels, false);
    SoapySDRDevice_setGain      (sdr, SOAPY_SDR_RX, *channels, RX_GAIN);
    
    if (tx_mode) {
        SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_TX, 0, SAMPLE_RATE);
        SoapySDRDevice_setFrequency (sdr, SOAPY_SDR_TX, 0, tx_freq, NULL);
        SoapySDRDevice_setGain      (sdr, SOAPY_SDR_TX, *channels, TX_GAIN);
    }

    if (rx_mode) {
        rxStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CS16, channels, channel_count, NULL);
        if (!rxStream) { fprintf(stderr, "RX stream: %s\n", SoapySDRDevice_lastError()); goto cleanup; }
        SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0);
        rx_mtu = SoapySDRDevice_getStreamMTU(sdr, rxStream);
        rx_buffer = (int16_t *)malloc(2 * rx_mtu * sizeof(int16_t));
        printf("RX MTU: %zu сэмплов\n", rx_mtu);
    }

    if (tx_mode) {
        txStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_TX, SOAPY_SDR_CS16, channels, channel_count, NULL);
        if (!txStream) { fprintf(stderr, "TX stream: %s\n", SoapySDRDevice_lastError()); goto cleanup; }
        SoapySDRDevice_activateStream(sdr, txStream, 0, 0, 0);
        tx_mtu = SoapySDRDevice_getStreamMTU(sdr, txStream);
        tx_buff = (int16_t *)malloc(2 * tx_mtu * sizeof(int16_t));
        printf("TX MTU: %zu сэмплов\n", tx_mtu);

        tx_args.sdr      = sdr;
        tx_args.txStream = txStream;
        tx_args.tx_mtu   = tx_mtu;
        tx_args.tx_buff  = tx_buff;
        pthread_create(&tx_thread, NULL, tx_thread_func, &tx_args);
    }

    signal(SIGINT,  stop_program);
    signal(SIGTERM, stop_program);

    if (rx_mode) {
        char filename[64];
        snprintf(filename, sizeof(filename), "%s_rxdata.pcm", sdr_id);
        file = fopen(filename, "wb");
        if (!file) { perror("fopen"); goto cleanup; }
        printf("Приём записывается в файл: %s\n", filename);
        printf("Начало работы (план: %d блоков по %zu сэмплов)...\n", RX_ITERATION_COUNT, rx_mtu);
        target_samples = RX_ITERATION_COUNT * rx_mtu;
    } else {
        printf("Начало передачи (бесконечно: 1 сигнал / 1 пауза, нажмите Ctrl+C для остановки)...\n");
        target_samples = 0;
    }

    while (running && (rx_mode ? (total_samples_read < target_samples) : true)) {
        if (rx_mode) {
            void *rx_buffs[] = {rx_buffer};
            int flags = 0;
            long long timeNs = 0;
            int sr = SoapySDRDevice_readStream(sdr, rxStream, rx_buffs, rx_mtu, &flags, &timeNs, timeoutUs);

            if (sr > 0) {
                if (file) fwrite(rx_buffer, 2 * sizeof(int16_t), (size_t)sr, file);
                
                total_samples_read += (size_t)sr;
                size_t current_block = total_samples_read / rx_mtu;
                
                if (current_block > last_block_printed) {
                    last_block_printed = current_block;
                    printf("Блок %zu/%d | Всего сэмплов: %zu\n", current_block, RX_ITERATION_COUNT, total_samples_read);
                }
            } else if (sr == SOAPY_SDR_TIMEOUT) {
                
            } else {
                fprintf(stderr, "RX Failed: %d\n", sr);
                goto cleanup;
            }
        } else {
            sleep(1);
        }
    }

    exit_code = EXIT_SUCCESS;
    printf("Работа успешно завершена.\n");

cleanup:
    running = 0;
    if (tx_mode) pthread_join(tx_thread, NULL);
    if (file) fclose(file);
    free(rx_buffer);
    free(tx_buff);
    if (sdr) {
        if (rxStream) { SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0); SoapySDRDevice_closeStream(sdr, rxStream); }
        if (txStream) { SoapySDRDevice_deactivateStream(sdr, txStream, 0, 0); SoapySDRDevice_closeStream(sdr, txStream); }
        SoapySDRDevice_unmake(sdr);
    }
    return exit_code;
}