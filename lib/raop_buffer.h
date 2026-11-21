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
 * modified by fduncanh 2021-23
 */

#ifndef RAOP_BUFFER_H
#define RAOP_BUFFER_H

#include "logger.h"
#include "raop_rtp.h"

typedef struct raop_buffer_s raop_buffer_t;

typedef int (*raop_resend_cb_t)(void *opaque, unsigned short seqno, unsigned short count);

raop_buffer_t *raop_buffer_init(logger_t *logger,
                                const unsigned char *aeskey,
                                const unsigned char *aesiv);
int raop_buffer_enqueue(raop_buffer_t *raop_buffer, unsigned char *data, unsigned short datalen, int use_seqnum);
void *raop_buffer_dequeue(raop_buffer_t *raop_buffer, unsigned int *length, uint32_t *rtp_timestamp, unsigned short *seqnum, int no_resend);
void raop_buffer_handle_resends(raop_buffer_t *raop_buffer, raop_resend_cb_t resend_cb, void *opaque);
void raop_buffer_flush(raop_buffer_t *raop_buffer, int next_seq);
/* Returns the monotonic residence time in milliseconds of the last dequeued
 * payload. This measures buffer waiting only, not network or speaker latency. */
uint64_t raop_buffer_last_queue_wait_ms(raop_buffer_t *raop_buffer);
/* Returns and clears the number of missing sequences skipped since the last
 * call. Writes the last skip's buffered-successor wait in milliseconds. */
uint64_t raop_buffer_take_gap_skips(raop_buffer_t *raop_buffer, uint64_t *wait_ms);
/* True when the playout head is missing but newer packets are present. */
int raop_buffer_has_gap(raop_buffer_t *raop_buffer);
/* Skip a stuck gap and preserve the earliest available received packet. */
void raop_buffer_recover(raop_buffer_t *raop_buffer);

int raop_buffer_decrypt(raop_buffer_t *raop_buffer, unsigned char *data, unsigned char* output,
                        unsigned int datalen, unsigned int *outputlen);
void raop_buffer_destroy(raop_buffer_t *raop_buffer);

#endif
