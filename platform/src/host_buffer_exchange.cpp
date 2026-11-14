/*
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * Host replacement for firmware/common/buffer_exchange.cpp.
 *
 * The device's BufferExchange::get() parks the consumer thread on the ChibiOS
 * scheduler and relies on the M4 core's IRQ to wake it (BufferExchange::
 * handle_isr -> chSchReadyI). The host producer (platform/src/host_baseband.cpp)
 * runs on an unrelated std::thread and does not share that scheduler lock, so a
 * wake-up posted between the FIFO check and the sleep is silently lost: the
 * consumer then sleeps forever and RecordView::stop()'s chThdWait() (a join)
 * never returns - the "window freezes on Stop" bug.
 *
 * It also ignores chThdShouldTerminate(), so even a correct wake-up cannot end
 * the join while the loop waits for a buffer that will never arrive.
 *
 * This replacement keeps the exact upstream behaviour of the FIFO protocol but
 * polls the queue with a short sleep instead of parking on the scheduler: it
 * cannot lose a wake-up, and it observes a termination request (RecordView::
 * stop -> ~CaptureThread -> chThdTerminate) promptly so the join completes.
 *
 * When a termination request is seen while no buffer is available, an empty
 * sentinel buffer is returned. CaptureThread::run() then performs a zero-length
 * write and re-queues it, observes chThdShouldTerminate() at the top of its
 * loop and returns, letting the destructor finish. The sentinel is a valid
 * StreamBuffer (non-null data, zero capacity) so no caller dereferences a null
 * pointer.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "buffer_exchange.hpp"

#include "ch.h"

BufferExchange* BufferExchange::obj{nullptr};

BufferExchange::BufferExchange(
    CaptureConfig* const config)  // : config_capture { config }
{
    obj = this;
    // In capture mode, baseband wants empty buffers, app waits for full buffers
    fifo_buffers_for_baseband = config->fifo_buffers_empty;
    fifo_buffers_for_application = config->fifo_buffers_full;
}

BufferExchange::BufferExchange(
    ReplayConfig* const config)  // : config_replay { config }
{
    obj = this;
    // In replay mode, baseband wants full buffers, app waits for empty buffers
    fifo_buffers_for_baseband = config->fifo_buffers_full;
    fifo_buffers_for_application = config->fifo_buffers_empty;
}

BufferExchange::~BufferExchange() {
    obj = nullptr;
    fifo_buffers_for_baseband = nullptr;
    fifo_buffers_for_application = nullptr;
}

namespace {

/* Zero-capacity buffer handed back when the loop is asked to terminate. Its
 * data pointer is non-null so the caller's write() is a well-defined no-op. */
uint8_t g_terminated_data[1];
StreamBuffer g_terminated_buffer{g_terminated_data, 0};

}  // namespace

StreamBuffer* BufferExchange::get(FIFO<StreamBuffer*>* fifo) {
    while (true) {
        StreamBuffer* p{nullptr};
        fifo->out(p);

        if (p) {
            return p;
        }

        /* A termination request may arrive while the producer is silent (e.g.
         * the baseband image has already stopped). Leave the loop with the
         * sentinel so the caller can observe the request and unwind. */
        if (chThdShouldTerminate()) {
            return &g_terminated_buffer;
        }

        /* Poll instead of parking on the scheduler; see the file header for why
         * a parked wait can lose the producer's wake-up on the host. */
        chThdSleepMicroseconds(200);
    }
}

StreamBuffer* BufferExchange::get_prefill(FIFO<StreamBuffer*>* fifo) {
    StreamBuffer* p{nullptr};
    fifo->out(p);

    return p;
}
