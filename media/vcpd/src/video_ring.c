#include "vcpd/video_ring.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

video_ring_t *video_ring_create(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return NULL;

    if (ftruncate(fd, (off_t)VIDEO_RING_MMAP_SIZE) < 0) {
        close(fd);
        return NULL;
    }

    void *m = mmap(NULL, VIDEO_RING_MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return NULL;

    video_ring_t *ring = (video_ring_t *)m;
    memset(ring, 0, VIDEO_RING_MMAP_SIZE);
    ring->magic    = VIDEO_RING_MAGIC;
    ring->capacity = VIDEO_RING_SLOTS;
    atomic_store_explicit(&ring->head, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
    return ring;
}

void video_ring_destroy(video_ring_t *ring, const char *path)
{
    if (ring)
        munmap(ring, VIDEO_RING_MMAP_SIZE);
    if (path)
        unlink(path);
}

bool video_ring_push(video_ring_t *ring, const void *data, uint32_t len)
{
    uint32_t head = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t next = (head + 1u) % VIDEO_RING_SLOTS;

    /* Full: next write position would collide with consumer */
    if (next == atomic_load_explicit(&ring->tail, memory_order_acquire))
        return false;

    video_ring_slot_t *slot = &ring->slots[head];
    if (len > VIDEO_RING_SLOT_MAX)
        len = VIDEO_RING_SLOT_MAX;
    slot->len = len;
    memcpy(slot->data, data, len);

    /* Publish the slot by advancing head */
    atomic_store_explicit(&ring->head, next, memory_order_release);
    return true;
}

bool video_ring_pop(video_ring_t *ring, void *buf, size_t cap, uint32_t *out_len)
{
    uint32_t tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);

    /* Empty */
    if (tail == atomic_load_explicit(&ring->head, memory_order_acquire))
        return false;

    video_ring_slot_t *slot = &ring->slots[tail];
    uint32_t len = slot->len;
    if (len > (uint32_t)cap)
        len = (uint32_t)cap;
    memcpy(buf, slot->data, len);
    *out_len = len;

    /* Release the slot by advancing tail */
    atomic_store_explicit(&ring->tail, (tail + 1u) % VIDEO_RING_SLOTS, memory_order_release);
    return true;
}
