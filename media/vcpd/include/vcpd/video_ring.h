#pragma once

/*
 * SPSC lock-free ring buffer backed by a memory-mapped file.
 *
 * Layout on disk / in mmap:
 *   [video_ring_header_t 64 bytes][video_ring_slot_t × VIDEO_RING_SLOTS]
 *
 * Usage:
 *   Producer thread calls video_ring_push() and writes 1 signal byte to the
 *   notification pipe so the consumer can poll() the pipe and then call
 *   video_ring_pop().  The pipe still carries its original EOF semantics for
 *   shutdown detection; the actual video data travels through the ring.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIDEO_RING_MAGIC    0x52494E47U  /* "RING" */
#define VIDEO_RING_SLOTS    32
#define VIDEO_RING_SLOT_MAX 65536        /* 64 KB per slot — fits any single VENC pack */
#define VIDEO_RING_FILE     "/dev/shm/vcpd_video_ring"

typedef struct {
    uint32_t           len;
    uint8_t            data[VIDEO_RING_SLOT_MAX];
} video_ring_slot_t;

/* Header sits at the start of the mmap'd region. */
typedef struct {
    uint32_t           magic;
    uint32_t           capacity;     /* = VIDEO_RING_SLOTS */
    _Atomic uint32_t   head;         /* producer: index of next slot to fill */
    _Atomic uint32_t   tail;         /* consumer: index of next slot to read */
    uint8_t            _pad[48];     /* pad header to 64 bytes */
    video_ring_slot_t  slots[VIDEO_RING_SLOTS];
} video_ring_t;

#define VIDEO_RING_MMAP_SIZE  ((size_t)sizeof(video_ring_t))

/*
 * Create and initialise a new ring file at `path`.
 * Returns a pointer to the mmap'd region, or NULL on failure.
 * The caller owns the mapping and must call video_ring_destroy() when done.
 */
video_ring_t *video_ring_create(const char *path);

/*
 * Unmap the ring and unlink the backing file.
 */
void video_ring_destroy(video_ring_t *ring, const char *path);

/*
 * Producer: copy `len` bytes from `data` into the next available slot.
 * Returns true on success, false if the ring is full (frame dropped).
 * Must be called from a single producer thread only.
 */
bool video_ring_push(video_ring_t *ring, const void *data, uint32_t len);

/*
 * Consumer: copy the oldest slot into `buf` (up to `cap` bytes).
 * Sets *out_len to the number of bytes written.
 * Returns false if the ring is empty.
 * Must be called from a single consumer thread only.
 */
bool video_ring_pop(video_ring_t *ring, void *buf, size_t cap, uint32_t *out_len);

/*
 * Consumer: copy the NEWEST slot into `buf`, skipping all older entries.
 * All skipped slots are freed for the producer immediately.
 * Use for real-time video: sender always gets the latest frame.
 * Returns false if the ring is empty.
 * Must be called from a single consumer thread only.
 */
bool video_ring_pop_latest(video_ring_t *ring, void *buf, size_t cap, uint32_t *out_len);
