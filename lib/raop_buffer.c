/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *==================================================================
 * modified by fduncanh 2021-2023
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "raop_buffer.h"
#include "raop_rtp.h"

#include "crypto.h"
#include "compat.h"
#include "stream.h"
#include "global.h"
#include "utils.h"
#include "byteutils.h"

#define RAOP_BUFFER_LENGTH 32
/* Storage capacity is separate from the bounded playout waiting window. */
#define RAOP_BUFFER_HISTORY_LENGTH 256
#define RAOP_GAP_WAIT_NS 350000000ULL
#define RAOP_RESEND_INTERVAL_NS 20000000ULL

/* Deadlines must not move when NTP or the Windows wall clock is corrected. */
static uint64_t buffer_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

typedef struct {
    /* Data available */
    int filled;

    uint64_t packet_arrival_time;

    /* RTP header */
    unsigned short seqnum;
    uint32_t rtp_timestamp;

    /* Payload data */
    unsigned int payload_size;
    void *payload_data;
} raop_buffer_entry_t;

struct raop_buffer_s {
    logger_t *logger;
    /* AES CTX used for decryption */
    aes_ctx_t *aes_ctx;

    /* First and last seqnum */
    int is_empty;
    unsigned short first_seqnum;
    unsigned short last_seqnum;
    uint64_t last_queue_wait_ms;
    uint64_t last_resend_ns;
    unsigned short last_resend_seq;
    int has_resend;
    uint64_t pending_gap_skips;
    uint64_t last_gap_wait_ms;

    /* RTP buffer entries */
    raop_buffer_entry_t entries[RAOP_BUFFER_HISTORY_LENGTH];
};

raop_buffer_t *
raop_buffer_init(logger_t *logger,
                 const unsigned char *aeskey,
                 const unsigned char *aesiv)
{
    assert(aeskey);
    assert(aesiv);
    raop_buffer_t *raop_buffer = (raop_buffer_t *) calloc(1, sizeof(raop_buffer_t));
    if (!raop_buffer) {
        return NULL;
    }
    raop_buffer->logger = logger;
    // Need to be initialized internally
    raop_buffer->aes_ctx = aes_cbc_init(aeskey, aesiv, AES_DECRYPT);

    for (int i = 0; i < RAOP_BUFFER_HISTORY_LENGTH; i++) {
        raop_buffer_entry_t *entry = &raop_buffer->entries[i];
        entry->payload_data = NULL;
        entry->payload_size = 0;
    }

    raop_buffer->is_empty = 1;

    return raop_buffer;
}

void
raop_buffer_destroy(raop_buffer_t *raop_buffer)
{
    for (int i = 0; i < RAOP_BUFFER_HISTORY_LENGTH; i++) {
        raop_buffer_entry_t *entry = &raop_buffer->entries[i];
        if (entry->payload_data != NULL) {
            free(entry->payload_data);
        }
    }

    if (raop_buffer) {
        aes_cbc_destroy(raop_buffer->aes_ctx);
        free(raop_buffer);
    }

}

static short
seqnum_cmp(unsigned short s1, unsigned short s2)
{
    return (s1 - s2);
}

int
raop_buffer_decrypt(raop_buffer_t *raop_buffer, unsigned char *data, unsigned char* output, unsigned int payload_size, unsigned int *outputlen)
{
    assert(raop_buffer);
    if (DECRYPTION_TEST) {
        char *str = utils_data_to_string(data,12,12);
        logger_log(raop_buffer->logger, LOGGER_INFO, "encrypted 12 byte header %s", str);
        free(str);
        if (payload_size) {
            str = utils_data_to_string(&data[12],16,16);
            logger_log(raop_buffer->logger, LOGGER_INFO, "len %d before decryption:\n%s", payload_size, str);
            free(str);
        }
    }
    int encryptedlen = payload_size / 16*16;
    memset(output, 0, payload_size);

    aes_cbc_decrypt(raop_buffer->aes_ctx, &data[12], output, encryptedlen);
    aes_cbc_reset(raop_buffer->aes_ctx);

    memcpy(output + encryptedlen, &data[12 + encryptedlen], payload_size - encryptedlen);
    *outputlen = payload_size;
    if (payload_size &&  DECRYPTION_TEST){
        switch (output[0]) {
        case 0x8c:
        case 0x8d:
        case 0x8e:
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x20:
	    break;
        default:
            logger_log(raop_buffer->logger, LOGGER_INFO, "***ERROR AUDIO FRAME  IS NOT AAC_ELD OR ALAC");
	    break;
        }
        if (DECRYPTION_TEST == 2) {
            logger_log(raop_buffer->logger, LOGGER_INFO, "decrypted audio frame, len = %d", *outputlen);
            char *str = utils_data_to_string(output,payload_size,16);
            logger_log(raop_buffer->logger, LOGGER_INFO,"%s",str);
            free(str);
        } else {
            char *str = utils_data_to_string(output,16,16);
            logger_log(raop_buffer->logger, LOGGER_INFO, "%d after  \n%s", payload_size, str);
            free(str);
        }
    }
    return 1;
}

int
raop_buffer_enqueue(raop_buffer_t *raop_buffer, unsigned char *data, unsigned short datalen, int use_seqnum) {
    unsigned char empty_packet_marker[] = { 0x00, 0x68, 0x34, 0x00 };
    assert(raop_buffer);

    /* Check packet data length is valid */
    if (datalen < 12 || datalen > RAOP_PACKET_LEN) {
        return -1;
    }
    /* Explicit no-data packets still occupy a sequence number. Keep a marker
     * so they cannot later be mistaken for packet loss and resent forever. */
    const int no_data = datalen == 12 ||
        (datalen == 16 && !memcmp(&data[12], empty_packet_marker, 4));
    int payload_size = datalen - 12;

    /* Get correct seqnum for the packet */
    unsigned short seqnum = 0;
    if (use_seqnum) {
        seqnum = byteutils_get_short_be(data, 2);
    } else {
        seqnum = raop_buffer->first_seqnum;
    }

    /* If this packet is too late, just skip it */
    if (!raop_buffer->is_empty && seqnum_cmp(seqnum, raop_buffer->first_seqnum) < 0) {
        return 0;
    }

    /* Check that there is always space in the buffer, otherwise flush */
    if (seqnum_cmp(seqnum, raop_buffer->first_seqnum + RAOP_BUFFER_HISTORY_LENGTH) >= 0) {
        raop_buffer_flush(raop_buffer, seqnum);
    }

    /* Get entry corresponding our seqnum */
    raop_buffer_entry_t *entry = &raop_buffer->entries[seqnum % RAOP_BUFFER_HISTORY_LENGTH];
    if (entry->filled && seqnum_cmp(entry->seqnum, seqnum) == 0) {
        /* Packet resend, we can safely ignore */
        return 0;
    }

    /* Update the raop_buffer entry header */
    entry->seqnum = seqnum;
    entry->rtp_timestamp = byteutils_get_int_be(data, 4);
    entry->filled = 1;
    entry->packet_arrival_time = buffer_now_ns();

    if (no_data) {
        entry->payload_data = NULL;
        entry->payload_size = 0;
    } else {
        entry->payload_data = malloc(payload_size);
        int decrypt_ret = raop_buffer_decrypt(raop_buffer, data, entry->payload_data, payload_size, &entry->payload_size);
        assert(decrypt_ret >= 0);
        assert((int) entry->payload_size <= payload_size);
    }

    /* Update the raop_buffer seqnums */
    if (raop_buffer->is_empty) {
        raop_buffer->first_seqnum = seqnum;
        raop_buffer->last_seqnum = seqnum;
        raop_buffer->is_empty = 0;
    }
    if (seqnum_cmp(seqnum, raop_buffer->last_seqnum) > 0) {
        raop_buffer->last_seqnum = seqnum;
    }
    return 1;
}

void *
raop_buffer_dequeue(raop_buffer_t *raop_buffer, unsigned int *length, uint32_t *rtp_timestamp, unsigned short *seqnum, int no_resend) {
    assert(raop_buffer);

    /* Calculate number of entries in the current buffer */
    short entry_count = seqnum_cmp(raop_buffer->last_seqnum, raop_buffer->first_seqnum)+1;

    /* Cannot dequeue from empty buffer */
    if (raop_buffer->is_empty || entry_count <= 0) {
        return NULL;
    }

    /* Get the first buffer entry for inspection */
    raop_buffer_entry_t *entry;
    while (entry_count > 0) {
        entry = &raop_buffer->entries[raop_buffer->first_seqnum % RAOP_BUFFER_HISTORY_LENGTH];
        if (entry->filled && entry->payload_size > 0) {
            break;
        }
        if (entry->filled) {
            entry->filled = 0;
            raop_buffer->first_seqnum++;
            entry_count--;
            continue;
        }
        /* A sparse stream may never fill the sequence window. Bound waiting
         * by the arrival age of buffered successors as well as distance. */
        uint64_t first_arrival = 0;
        for (int offset = 1; offset < entry_count; ++offset) {
            const raop_buffer_entry_t *newer = &raop_buffer->entries[
                (unsigned short)(raop_buffer->first_seqnum + offset) % RAOP_BUFFER_HISTORY_LENGTH];
            if (newer->filled && (!first_arrival || newer->packet_arrival_time < first_arrival)) {
                first_arrival = newer->packet_arrival_time;
            }
        }
        if (!no_resend && entry_count < RAOP_BUFFER_LENGTH) {
            if (!first_arrival || buffer_now_ns() - first_arrival < RAOP_GAP_WAIT_NS) {
                return NULL;
            }
        }
        raop_buffer->pending_gap_skips++;
        raop_buffer->last_gap_wait_ms = first_arrival
            ? (buffer_now_ns() - first_arrival) / 1000000ULL : 0;
        raop_buffer->first_seqnum += 1;
        entry_count--;
    }
    if (entry_count <= 0) {
        return NULL;
    }

    /* Update buffer and validate entry */
    raop_buffer->first_seqnum += 1;
    entry->filled = 0;
    raop_buffer->last_queue_wait_ms =
        (buffer_now_ns() - entry->packet_arrival_time) / 1000000ULL;

    /* Return entry payload buffer */
    *rtp_timestamp = entry->rtp_timestamp;
    *seqnum = entry->seqnum;
    *length = entry->payload_size;
    entry->payload_size = 0;
    void* data = entry->payload_data;
    entry->payload_data = NULL;
    return data;
}

uint64_t raop_buffer_last_queue_wait_ms(raop_buffer_t *raop_buffer) {
    return raop_buffer->last_queue_wait_ms;
}

uint64_t raop_buffer_take_gap_skips(raop_buffer_t *raop_buffer, uint64_t *wait_ms) {
    const uint64_t count = raop_buffer->pending_gap_skips;
    *wait_ms = raop_buffer->last_gap_wait_ms;
    raop_buffer->pending_gap_skips = 0;
    return count;
}

void raop_buffer_handle_resends(raop_buffer_t *raop_buffer, raop_resend_cb_t resend_cb, void *opaque) {
    assert(raop_buffer);
    assert(resend_cb);

    if (seqnum_cmp(raop_buffer->first_seqnum, raop_buffer->last_seqnum) < 0) {
        unsigned short seqnum, count = 0;
        for (seqnum = raop_buffer->first_seqnum; seqnum_cmp(seqnum, raop_buffer->last_seqnum) < 0; seqnum++) {
            raop_buffer_entry_t *entry = &raop_buffer->entries[seqnum % RAOP_BUFFER_HISTORY_LENGTH];
            if (entry->filled) {
                break;
            }
	    count++;
        }
        if (count){
            const uint64_t now = buffer_now_ns();
            if (raop_buffer->has_resend && raop_buffer->last_resend_seq == raop_buffer->first_seqnum
                && now - raop_buffer->last_resend_ns < RAOP_RESEND_INTERVAL_NS) {
                return;
            }
            raop_buffer->has_resend = 1;
            raop_buffer->last_resend_seq = raop_buffer->first_seqnum;
            raop_buffer->last_resend_ns = now;
            logger_log(raop_buffer->logger, LOGGER_DEBUG,
                       "raop_buffer_handle_resends first_seqnum=%u last seqnum=%u",
                       raop_buffer->first_seqnum, raop_buffer->last_seqnum);
            resend_cb(opaque, raop_buffer->first_seqnum, count);
        }
    }
}

void raop_buffer_flush(raop_buffer_t *raop_buffer, int next_seq) {
    assert(raop_buffer);
    raop_buffer->has_resend = 0;
    raop_buffer->last_queue_wait_ms = 0;
    raop_buffer->pending_gap_skips = 0;
    raop_buffer->last_gap_wait_ms = 0;

    for (int i = 0; i < RAOP_BUFFER_HISTORY_LENGTH; i++) {
        if (raop_buffer->entries[i].payload_data) {
            free(raop_buffer->entries[i].payload_data);
            raop_buffer->entries[i].payload_data = NULL;   
            raop_buffer->entries[i].payload_size = 0;
        }
        raop_buffer->entries[i].filled = 0;
    }
    if (next_seq < 0 || next_seq > 0xffff) {
        raop_buffer->is_empty = 1;
    } else {
        raop_buffer->first_seqnum = next_seq;
        raop_buffer->last_seqnum = next_seq - 1;
    }
}

void raop_buffer_recover(raop_buffer_t *raop_buffer) {
    assert(raop_buffer);
    if (raop_buffer->is_empty) {
        return;
    }
    /* Keep received audio: recovery advances past holes instead of discarding
     * the entire backlog and waiting for yet another packet. */
    while (seqnum_cmp(raop_buffer->last_seqnum, raop_buffer->first_seqnum) >= 0) {
        const raop_buffer_entry_t *entry = &raop_buffer->entries[
            raop_buffer->first_seqnum % RAOP_BUFFER_HISTORY_LENGTH];
        if (entry->filled) {
            return;
        }
        raop_buffer->first_seqnum++;
    }
}

int raop_buffer_has_gap(raop_buffer_t *raop_buffer) {
    assert(raop_buffer);
    if (raop_buffer->is_empty) {
        return 0;
    }
    if (seqnum_cmp(raop_buffer->last_seqnum, raop_buffer->first_seqnum) <= 0) {
        return 0;
    }
    const raop_buffer_entry_t *entry =
        &raop_buffer->entries[raop_buffer->first_seqnum %
                              RAOP_BUFFER_HISTORY_LENGTH];
    return !entry->filled;
}
