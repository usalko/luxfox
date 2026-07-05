#include "radiod/tx_scheduler.h"
#include "radiod/ipc.h"
#include "ulama/ulama_frame.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_tests;

static void check(int ok, const char *expr, const char *file, int line)
{
	g_tests++;
	if (ok)
		return;
	g_failures++;
	fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
}

#define CHECK(cond) check(!!(cond), #cond, __FILE__, __LINE__)

/* Build one wire-packed ULAMA video fragment: seq identifies the frame,
 * frag_idx/frag_total identify its position, last=true sets
 * ULAMA_FLAG_LAST_FRAGMENT. Mirrors vcpd's tx_video_frame_pass(). */
static size_t make_fragment(uint8_t *out, size_t out_cap, uint16_t seq,
			   uint8_t frag_idx, uint8_t frag_total, bool last)
{
	uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	ulama_frame_view_t uf = {
		.src_node = 1,
		.dst_node = 254,
		.flags = (uint8_t)(ULAMA_FLAG_FRAGMENT | (last ? ULAMA_FLAG_LAST_FRAGMENT : 0)),
		.traffic_class = ULAMA_CLASS_VIDEO,
		.seq = seq,
		.frag_idx = frag_idx,
		.frag_total = frag_total,
		.ttl = ULAMA_FRAME_DEFAULT_TTL,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	size_t len = 0;
	if (!ulama_frame_pack(&uf, out, out_cap, &len))
		return 0;
	return len;
}

/* Enqueue every fragment of a frame_total-fragment frame with the given seq. */
static void enqueue_whole_frame(radio_tx_scheduler_t *sched, uint16_t seq,
				uint8_t frag_total)
{
	for (uint8_t i = 0; i < frag_total; i++) {
		uint8_t buf[64];
		size_t len = make_fragment(buf, sizeof(buf), seq, i, frag_total,
					   i + 1 == frag_total);
		CHECK(len > 0);
		CHECK(radio_tx_enqueue(sched, RADIO_PRIO_VIDEO, 0, buf, len) == 0);
	}
}

static uint16_t dequeue_frame_seq(radio_tx_scheduler_t *sched)
{
	uint8_t prio;
	const radio_tx_slot_t *slot = radio_tx_dequeue(sched, &prio);
	if (slot == NULL)
		return 0xFFFF;
	CHECK(prio == RADIO_PRIO_VIDEO);
	CHECK(slot->is_fragment);
	return slot->frame_seq;
}

/* ---- Test: normal fill-to-capacity still enqueues everything ---- */

static void test_fills_to_capacity(void)
{
	radio_tx_scheduler_t sched;
	radio_tx_scheduler_init(&sched);

	/* One fragment per frame, RADIO_TX_QUEUE_SIZE frames exactly fill it. */
	for (int i = 0; i < RADIO_TX_QUEUE_SIZE; i++)
		enqueue_whole_frame(&sched, (uint16_t)i, 1);

	CHECK(radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) == RADIO_TX_QUEUE_SIZE);
	CHECK(dequeue_frame_seq(&sched) == 0);
}

/* ---- Test: CTRL/TELEM keep reject-newest on overflow (unchanged) ---- */

static void test_ctrl_rejects_on_full(void)
{
	radio_tx_scheduler_t sched;
	radio_tx_scheduler_init(&sched);

	uint8_t payload[4] = {0xAA, 0xBB, 0xCC, 0xDD};
	ulama_frame_view_t uf = {
		.src_node = 1, .dst_node = 254,
		.traffic_class = ULAMA_CLASS_CTRL,
		.ttl = ULAMA_FRAME_DEFAULT_TTL,
		.payload = payload, .payload_len = sizeof(payload),
	};
	uint8_t buf[64];
	size_t len = 0;
	CHECK(ulama_frame_pack(&uf, buf, sizeof(buf), &len));

	for (int i = 0; i < RADIO_TX_QUEUE_SIZE; i++)
		CHECK(radio_tx_enqueue(&sched, RADIO_PRIO_CTRL, 0, buf, len) == 0);

	/* Queue full: new CTRL packet must be rejected, not evict anything. */
	CHECK(radio_tx_enqueue(&sched, RADIO_PRIO_CTRL, 0, buf, len) == -1);
	CHECK(radio_tx_queue_depth(&sched, RADIO_PRIO_CTRL) == RADIO_TX_QUEUE_SIZE);
}

/*
 * The core regression test: a "clean" (fully queued, nothing dequeued yet)
 * old frame must be evicted ATOMICALLY — all its fragments together — not
 * one fragment at a time. A single-fragment evict would leave the frame's
 * remaining fragments in the queue advertising a frag_total the receiver can
 * never complete correctly (or, worse, complete with a hole in the middle:
 * exactly the "half frame" corruption this eviction policy replaces).
 */
static void test_clean_old_frame_evicted_atomically(void)
{
	radio_tx_scheduler_t sched;
	radio_tx_scheduler_init(&sched);

	/* Fill the queue with a single old 4-fragment frame (seq=1) followed
	 * by filler single-fragment frames until full. */
	enqueue_whole_frame(&sched, 1, 4);
	for (int i = 0; radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) < RADIO_TX_QUEUE_SIZE; i++)
		enqueue_whole_frame(&sched, (uint16_t)(100 + i), 1);

	int depth_before = radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO);
	CHECK(depth_before == RADIO_TX_QUEUE_SIZE);

	/* Force an overflow with a new frame; the oldest frame (seq=1, still
	 * fully untouched) must be evicted as a whole — all 4 fragments gone,
	 * not just 1 — never leaving seq=1 fragments 1..3 behind alone. */
	enqueue_whole_frame(&sched, 999, 1);

	for (uint8_t i = 0; i < RADIO_TX_QUEUE_SIZE; i++) {
		uint8_t prio;
		const radio_tx_slot_t *slot = radio_tx_dequeue(&sched, &prio);
		if (slot == NULL)
			break;
		CHECK(slot->frame_seq != 1);
	}
}

/*
 * A frame that has already started transmitting (dirty: some fragments
 * dequeued, its remaining ones still queued) must never be partially
 * evicted. New arrivals are rejected instead until it finishes draining.
 */
static void test_dirty_frame_protected_from_eviction(void)
{
	radio_tx_scheduler_t sched;
	radio_tx_scheduler_init(&sched);

	enqueue_whole_frame(&sched, 1, 4);
	for (int i = 0; radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) < RADIO_TX_QUEUE_SIZE; i++)
		enqueue_whole_frame(&sched, (uint16_t)(100 + i), 1);

	/* Dequeue frag 0/4 of seq=1 — the TDMA slot loop already sent it on
	 * the air. seq=1 is now "dirty": 3 fragments still queued behind it. */
	CHECK(dequeue_frame_seq(&sched) == 1);
	CHECK(radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) == RADIO_TX_QUEUE_SIZE - 1);

	/* Queue isn't full anymore, so this should still succeed normally... */
	enqueue_whole_frame(&sched, 999, 1);
	CHECK(radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) == RADIO_TX_QUEUE_SIZE);

	/* ...but now it IS full again, and the oldest queued fragment belongs
	 * to the DIRTY frame (seq=1, frags 1..3 remaining) — must be rejected,
	 * not shredded. */
	uint8_t buf[64];
	size_t len = make_fragment(buf, sizeof(buf), 1000, 0, 1, true);
	CHECK(radio_tx_enqueue(&sched, RADIO_PRIO_VIDEO, 0, buf, len) == -1);

	/* seq=1's remaining fragments 1,2,3 must still all be present, in order. */
	CHECK(dequeue_frame_seq(&sched) == 1);
	CHECK(dequeue_frame_seq(&sched) == 1);
	CHECK(dequeue_frame_seq(&sched) == 1);
}

/* Once the dirty frame's last fragment drains, it stops being protected. */
static void test_dirty_clears_after_last_fragment(void)
{
	radio_tx_scheduler_t sched;
	radio_tx_scheduler_init(&sched);

	enqueue_whole_frame(&sched, 1, 2);
	CHECK(dequeue_frame_seq(&sched) == 1);  /* frag 0: now dirty */
	CHECK(dequeue_frame_seq(&sched) == 1);  /* frag 1 (last): dirty clears */

	for (int i = 0; radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) < RADIO_TX_QUEUE_SIZE; i++)
		enqueue_whole_frame(&sched, (uint16_t)(100 + i), 1);

	/* No dirty frame anymore, so overflow eviction must succeed normally. */
	uint8_t buf[64];
	size_t len = make_fragment(buf, sizeof(buf), 999, 0, 1, true);
	CHECK(radio_tx_enqueue(&sched, RADIO_PRIO_VIDEO, 0, buf, len) == 0);
	CHECK(radio_tx_queue_depth(&sched, RADIO_PRIO_VIDEO) == RADIO_TX_QUEUE_SIZE);
}

int main(void)
{
	const struct { const char *name; void (*fn)(void); } tests[] = {
		{"fills_to_capacity", test_fills_to_capacity},
		{"ctrl_rejects_on_full", test_ctrl_rejects_on_full},
		{"clean_old_frame_evicted_atomically", test_clean_old_frame_evicted_atomically},
		{"dirty_frame_protected_from_eviction", test_dirty_frame_protected_from_eviction},
		{"dirty_clears_after_last_fragment", test_dirty_clears_after_last_fragment},
	};

	size_t n = sizeof(tests) / sizeof(tests[0]);
	for (size_t i = 0; i < n; i++) {
		int before = g_failures;
		tests[i].fn();
		fprintf(stderr, "  %s: %s\n", tests[i].name,
			g_failures == before ? "OK" : "FAILED");
	}

	fprintf(stderr, "\ntest_tx_scheduler: %d tests, %d failures\n",
		g_tests, g_failures);
	return g_failures != 0 ? 1 : 0;
}
