/*
 * What llcapture.so (inside GNOME Shell) tells llbridge about the frames of the screen cast.
 *
 * Mutter hands every frame to the stream with the list of what changed in it. For a stream into plain
 * memory that list is wrong under load: a frame painted while the previous one still waits to be
 * recorded leaves no trace in it. llcapture.so sees every frame being painted and keeps the true
 * lists here, in the file $XDG_RUNTIME_DIR/llcapture that both programs map.
 *
 * A buffer of the stream is matched to its record by time: the timestamp Mutter puts into the buffer
 * is taken a moment before llcapture.so notes the record.
 */
#ifndef LLCAPTURE_H
#define LLCAPTURE_H

#include <stdint.h>

#define LLCAPTURE_FILE "llcapture"
#define LLCAPTURE_MAGIC 0x31434c4c /* "LLC1" */
#define LLCAPTURE_FRAMES 64
#define LLCAPTURE_RECORDS 64
#define LLCAPTURE_RECTS 6

struct llcapture_rect {
    int32_t x, y, w, h;
};

/* what one painted frame changed, in pixels of the monitor */
struct llcapture_frame {
    int32_t count; /* -1: all of it */
    struct llcapture_rect rects[LLCAPTURE_RECTS];
};

/* one frame handed to a stream */
struct llcapture_record {
    int64_t time_us; /* CLOCK_MONOTONIC */
    uint64_t frame;  /* the latest frame painted by then */
};

struct llcapture_shared {
    uint32_t magic;
    uint32_t pid;
    uint64_t frame;   /* the latest frame painted; frames[frame % LLCAPTURE_FRAMES] is its entry */
    uint64_t records; /* records made; record[(records - 1) % LLCAPTURE_RECORDS] is the latest */
    struct llcapture_frame frames[LLCAPTURE_FRAMES];
    struct llcapture_record record[LLCAPTURE_RECORDS];
};

#endif
