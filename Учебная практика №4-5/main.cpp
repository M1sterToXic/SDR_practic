#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#define SAMPLE_RATE 1000000.0

#define RX_FREQ_SDR1 890000000.0
#define TX_FREQ_SDR1 800000000.0

#define RX_FREQ_SDR2 800000000.0
#define TX_FREQ_SDR2 890000000.0

#define RX_GAIN 30.0
#define TX_GAIN 10.0

#define ITERATION_COUNT 100
#define TIMEOUT_US 400000

static volatile sig_atomic_t running = 1;

static void stop_program(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        return EXIT_FAILURE;
    }

    const char *sdr_id = argv[1];
    const char *mode = argv[2];
    const char *uri = argv[3];

    bool sdr1 = (strcmp(sdr_id, "sdr1") == 0);
    bool sdr2 = (strcmp(sdr_id, "sdr2") == 0);
    bool rx_mode = (strcmp(mode, "rx") == 0);
    bool tx_mode = (strcmp(mode, "tx") == 0);

    if ((!sdr1 && !sdr2) || (!rx_mode && !tx_mode)) {
        return EXIT_FAILURE;
    }

    double rx_freq;
    double tx_freq;
    if (sdr1) {
        rx_freq = RX_FREQ_SDR1;
        tx_freq = TX_FREQ_SDR1;
    } else {
        rx_freq = RX_FREQ_SDR2;
        tx_freq = TX_FREQ_SDR2;
    }

    printf("Устройство: %s\n", sdr_id);
    printf("Режим: %s\n", rx_mode ? "RX" : "TX");
    printf("RX частота: %.3f МГц\n", rx_freq / 1e6);
    if (tx_mode) {
        printf("TX частота: %.3f МГц\n", tx_freq / 1e6);
    }

    SoapySDRDevice *sdr = NULL;
    SoapySDRStream *rxStream = NULL;
    SoapySDRStream *txStream = NULL;
    int16_t *rx_buffer = NULL;
    int16_t *tx_buff = NULL;
    FILE *file = NULL;
    int exit_code = EXIT_FAILURE;
    size_t rx_mtu = 0;
    size_t tx_mtu = 0;
    size_t channels[] = {0};
    long long last_time = 0;
    size_t buffers_read = 0;
    const long timeoutUs = TIMEOUT_US;

    SoapySDRKwargs args = {};
    SoapySDRKwargs_set(&args, "driver", "plutosdr");
    SoapySDRKwargs_set(&args, "uri", uri);
    SoapySDRKwargs_set(&args, "direct", "1");
    SoapySDRKwargs_set(&args, "timestamp_every", "1920");
    SoapySDRKwargs_set(&args, "loopback", "0");

    sdr = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);

    if (sdr == NULL) {
        fprintf(stderr, "Ошибка: не удалось открыть SDR (%s): %s\n", uri, SoapySDRDevice_lastError());
        return EXIT_FAILURE;
    }

    printf("Устройство успешно открыто\n");

    if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, SAMPLE_RATE) != 0) {
        fprintf(stderr, "Ошибка настройки RX sample rate: %s\n", SoapySDRDevice_lastError());
        goto cleanup;
    }
    if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, rx_freq, NULL) != 0) {
        fprintf(stderr, "Ошибка настройки RX частоты: %s\n", SoapySDRDevice_lastError());
        goto cleanup;
    }
    if (SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, RX_GAIN) != 0) {
        fprintf(stderr, "Предупреждение: ошибка настройки RX gain: %s\n", SoapySDRDevice_lastError());
    }

    if (tx_mode) {
        if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_TX, 0, SAMPLE_RATE) != 0) {
            fprintf(stderr, "Ошибка настройки TX sample rate: %s\n", SoapySDRDevice_lastError());
            goto cleanup;
        }
        if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_TX, 0, tx_freq, NULL) != 0) {
            fprintf(stderr, "Ошибка настройки TX частоты: %s\n", SoapySDRDevice_lastError());
            goto cleanup;
        }
        if (SoapySDRDevice_setGain(sdr, SOAPY_SDR_TX, 0, TX_GAIN) != 0) {
            fprintf(stderr, "Предупреждение: ошибка настройки TX gain: %s\n", SoapySDRDevice_lastError());
        }
    }

    rxStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CS16, channels, 1, NULL);
    if (rxStream == NULL) {
        fprintf(stderr, "Ошибка создания RX потока: %s\n", SoapySDRDevice_lastError());
        goto cleanup;
    }
    if (SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0) != 0) {
        fprintf(stderr, "Ошибка активации RX потока: %s\n", SoapySDRDevice_lastError());
        goto cleanup;
    }

    rx_mtu =
SoapySDRDevice_getStreamMTU(sdr, rxStream);
    if (rx_mtu == 0) {
        fprintf(stderr, "Ошибка: RX MTU равен нулю\n");
        goto cleanup;
    }
    printf("RX MTU: %zu сэмплов\n", rx_mtu);

    rx_buffer = (int16_t *)malloc(2 * rx_mtu * sizeof(int16_t));
    if (rx_buffer == NULL) {
        fprintf(stderr, "Ошибка выделения памяти для RX буфера\n");
        goto cleanup;
    }

    if (tx_mode) {
        txStream = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_TX, SOAPY_SDR_CS16, channels, 1, NULL);
        if (txStream == NULL) {
            fprintf(stderr, "Ошибка создания TX потока: %s\n", SoapySDRDevice_lastError());
            goto cleanup;
        }
        if (SoapySDRDevice_activateStream(sdr, txStream, 0, 0, 0) != 0) {
            fprintf(stderr, "Ошибка активации TX потока: %s\n", SoapySDRDevice_lastError());
            goto cleanup;
        }

        tx_mtu = SoapySDRDevice_getStreamMTU(sdr, txStream);
        if (tx_mtu == 0) {
            fprintf(stderr, "Ошибка: TX MTU равен нулю\n");
            goto cleanup;
        }
        printf("TX MTU: %zu сэмплов\n", tx_mtu);

        tx_buff = (int16_t *)malloc(2 * tx_mtu * sizeof(int16_t));
        if (tx_buff == NULL) {
            fprintf(stderr, "Ошибка выделения памяти для TX буфера\n");
            goto cleanup;
        }
    }

    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    if (rx_mode) {
        char filename[64];
        snprintf(filename, sizeof(filename), "%s_rxdata.pcm", sdr_id);
        file = fopen(filename, "wb");
        if (file == NULL) {
            perror("Не удалось открыть файл для записи");
            goto cleanup;
        }
        printf("Приём записывается в файл: %s\n", filename);
    }

    printf("Начало работы (план: %d буферов)...\n", ITERATION_COUNT);

    while (running && buffers_read < ITERATION_COUNT) {
        void *rx_buffs[] = {rx_buffer};
        int flags = 0;
        long long timeNs = 0;
        int sr = SoapySDRDevice_readStream(sdr, rxStream, rx_buffs, rx_mtu, &flags, &timeNs, timeoutUs);
        if (sr > 0) {
            if (rx_mode && file != NULL) {
                size_t written = fwrite(rx_buffer, 2 * sizeof(int16_t), (size_t)sr, file);
                if (written != (size_t)sr) {
                    fprintf(stderr, "Ошибка записи в файл\n");
                    goto cleanup;
                }
            }

            if (flags & SOAPY_SDR_HAS_TIME) {
                printf("Буфер %zu/%d | Сэмплы: %d | Флаги: %d | Время: %lld нс | Разница: %lld нс\n",
                       buffers_read + 1, ITERATION_COUNT, sr, flags, timeNs, timeNs - last_time);
                last_time = timeNs;
            } else {
                printf("Буфер %zu/%d | Сэмплы: %d | Флаги: %d | Время: н/д\n",
                       buffers_read + 1, ITERATION_COUNT, sr, flags);
            }

            if (tx_mode) {
                size_t phase = buffers_read % 40;
                bool tx_on = (phase >= 2 && phase < 22);

                if (phase == 2) {
                    printf("=== НАЧАЛО ПАКЕТА (буфер %zu) ===\n", buffers_read);
                } else if (phase == 22) {
                    printf("===КОНЕЦ ПАКЕТА (буфер %zu) ===\n", buffers_read);
                }

                memset(tx_buff, 0, 2 * tx_mtu * sizeof(int16_t));

                if (tx_on) {
                    for (size_t i = 0; i < 200 && i < tx_mtu; i++) {
                        tx_buff[2*i]     = 2000 << 4;
                        tx_buff[2*i + 1] = 2000 << 4;
                    }
                }

                void *tx_buffs[] = {tx_buff};
                int tx_flags = 0;
                long long tx_time = 0;

                int st = SoapySDRDevice_writeStream(
                    sdr, txStream,
                    (const void * const *)tx_buffs,
                    tx_mtu, &tx_flags, tx_time, timeoutUs);

                if (st < 0 || (size_t)st != tx_mtu) {
                    fprintf(stderr, "Ошибка передачи (TX Failed): %d\n", st);
                }
            }

            buffers_read++;
        } else if (sr == SOAPY_SDR_TIMEOUT) {
            printf("Таймаут чтения потока RX, повтор...\n");
        } else {
            fprintf(stderr, "Ошибка чтения RX потока: %d\n", sr);
            goto cleanup;
        }
    }

    exit_code = EXIT_SUCCESS;
    printf("Работа успешно завершена.\n");

cleanup:
    if (file != NULL) {
        fclose(file);
    }

    free(rx_buffer);
    free(tx_buff);

    if (sdr != NULL) {
        if (rxStream != NULL) {
            SoapySDRDevice_deactivateStream(sdr, rxStream, 0, 0);
            SoapySDRDevice_closeStream(sdr, rxStream);
        }
        if (txStream != NULL) {
            SoapySDRDevice_deactivateStream(sdr, txStream, 0, 0);
            SoapySDRDevice_closeStream(sdr, txStream);
        }
        SoapySDRDevice_unmake(sdr);
    }

    return exit_code;
}