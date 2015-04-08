#pragma once

// Rename types for convenience
typedef int8_t      int8;
typedef uint8_t     uint8;
typedef int16_t     int16;
typedef uint16_t    uint16;
typedef int32_t     int32;
typedef uint32_t    uint32;
typedef int64_t     int64;
typedef uint64_t    uint64;
typedef int32_t     bool32;

#include "vector.generated.h"  // Generated by metaprogram

typedef struct Rectl_s
{
    v2l top_left;
    v2l bot_right;
} Rectl;

typedef struct Brush_s
{
    int64 view_scale;
    int64 radius;  // This should be replaced by a BrushType and some union containing brush info.
} Brush;

typedef struct RasterBrush_s
{
    Rectl bounds;
    v2l size;
    uint8* bitmask;
    size_t bitmask_size;
} RasterBrush;

typedef struct Stroke_s
{
    v2l*        points;
    int64       num_points;
    Brush       brush;
} Stroke;

typedef struct MiltonState_s
{
    int32_t     full_width;             // Dimensions of the raster
    int32_t     full_height;
    uint8_t     bytes_per_pixel;
    uint8_t*    raster_buffer;
    size_t      raster_buffer_size;

    v2l screen_size;

    // Maps screen_size to a rectangle in our infinite canvas.
    int64 view_scale;

    // Current stroke.
    v2l stroke_points[4096];
    int64       num_stroke_points;

    // Before we get our nice spacial partition...
    Stroke    stored_strokes[4096];
    int64           num_stored_strokes;

    // Heap
    Arena*      root_arena;         // Persistent memory.
    Arena*      transient_arena;    // Gets reset after every call to milton_update().
    // Debug:
} MiltonState;

typedef struct MiltonInput_s
{
    bool32 full_refresh;
    bool32 reset;
    v2l* brush;
    int scale;
} MiltonInput;

static void milton_init(MiltonState* milton_state)
{
    // Allocate enough memory for the maximum possible supported resolution. As
    // of now, it seems like future 8k displays will adopt this resolution.
    milton_state->full_width      = 7680;
    milton_state->full_height     = 4320;
    milton_state->bytes_per_pixel = 4;
    milton_state->view_scale      = ((int64)1 << 10);
    // A view_scale of a billion puts the initial scale at one meter.

    int closest_power_of_two = (1 << 27);  // Ceiling of log2(width * height * bpp)
    milton_state->raster_buffer_size = closest_power_of_two;

    milton_state->raster_buffer = arena_alloc_array(milton_state->root_arena,
            milton_state->raster_buffer_size, uint8_t);
}

static Rectl bounding_rect_for_stroke(v2l points[], int64 num_points)
{
    assert (num_points > 0);

    v2l top_left = points[0];
    v2l bot_right = points[0];

    for (int64 i = 1; i < num_points; ++i)
    {
        v2l point = points[i];
        if (point.x < top_left.x) top_left.x = point.x;
        if (point.y > top_left.y) top_left.x = point.x;
        if (point.x > bot_right.x) bot_right.x = point.x;
        if (point.y > bot_right.y) bot_right.y = point.y;
    }
    Rectl rect = { top_left, bot_right };
    return rect;
}

    // Move from infinite canvas to raster
inline static v2l canvas_to_raster(MiltonState* milton_state, v2l canvas_point)
{
    v2l screen_center = invscale_v2l(milton_state->screen_size, 2);
    v2l point = canvas_point;
    point = invscale_v2l(point, milton_state->view_scale);
    point = add_v2l     ( point, screen_center );
    return point;
}

    // Move to infinite canvas
inline static v2l raster_to_canvas(MiltonState* milton_state, v2l raster_point)
{
    v2l screen_center = invscale_v2l(milton_state->screen_size, 2);
    v2l canvas_point = raster_point;
    canvas_point = sub_v2l   ( canvas_point ,  screen_center );
    canvas_point = scale_v2l (canvas_point, milton_state->view_scale);
    return canvas_point;
}

static RasterBrush rasterize_brush(Arena* transient_arena, const Brush brush, float scale)
{
    RasterBrush rbrush;

    const int64 radius = (int64)(brush.radius * scale);

    if (radius > 500 || radius == 0)
    {
        rbrush.bitmask = 0;
        return rbrush;
    }

    Rectl bounds = (Rectl){0};
    bounds.top_left = (v2l){ -radius , radius };
    bounds.bot_right = (v2l){ radius, -radius };

    rbrush.bounds = bounds;
    rbrush.size = (v2l) { 2 * radius, 2 * radius };
    int64 radius2 = radius * radius;

    size_t size = rbrush.size.w * rbrush.size.h ;
    uint8* bitmask = arena_alloc_array(transient_arena, size, uint8);

    rbrush.bitmask_size = size;
    rbrush.bitmask = bitmask;

    for (int64 i = -radius; i < radius; ++i)
    {
        for (int64 j = -radius; j < radius; ++j)
        {
            int64 index = (j + radius) * rbrush.size.w + (i + radius);
            assert(index < (int64)size);
            if ((i * i + j * j) < radius2)
            {
                // write 1 to mask
                bitmask[index] = 1;
            }
            else
            {
                // write 0 to mask
                bitmask[index] = 0;
            }
        }
    }
    return rbrush;
}

static void rasterize_stroke(MiltonState* milton_state, const Brush brush, v2l* points, int64 num_points)
{
    uint32* pixels = (uint32_t*)milton_state->raster_buffer;

    const float relative_scale = (float)brush.view_scale / (float)milton_state->view_scale;
    RasterBrush rbrush = rasterize_brush(milton_state->transient_arena, brush, relative_scale);

    if (!rbrush.bitmask) return;

    for (int64 i = 0; i < num_points; ++i)
    {
        v2l canvas_point = points[i];

        v2l base_point = canvas_to_raster(milton_state, canvas_point);

        int64 base_index = base_point.y * milton_state->screen_size.w + base_point.x;

        const int64 h_limit = min(rbrush.size.h, milton_state->screen_size.h) + base_point.y;
        const int64 w_limit = min(rbrush.size.w, milton_state->screen_size.w) + base_point.x;

        if (base_point.y >= milton_state->screen_size.h || base_point.x >= milton_state->screen_size.w)
            continue;
        for (int64 y = base_point.y; y < h_limit; ++y)
        {
            for (int64 x = base_point.x; x < w_limit; ++x)
            {
                int64 bitmask_x = rbrush.bounds.top_left.x + x;
                int64 bitmask_y = rbrush.bounds.bot_right.y + y;

                if ( bitmask_x >= 0 && bitmask_y >= 0)
                {
                    bitmask_x -= base_point.x;
                    bitmask_y -= base_point.y;

                    int64 bitmask_index =
                        (bitmask_y + rbrush.size.h / 2) * rbrush.size.w +
                        (bitmask_x + rbrush.size.w / 2);


                    assert (bitmask_index < (int64)rbrush.bitmask_size);
                    uint8 bit_value = rbrush.bitmask[bitmask_index];

                    int64 index = base_index +
                        bitmask_y * milton_state->screen_size.w +
                        bitmask_x;

                    if (index < 0) continue;

                    assert ( milton_state->raster_buffer_size < ((uint64)1 << 63));
                    assert (index < (int64)milton_state->raster_buffer_size);
                    if (bit_value)
                        pixels[index] = 0xff00ffff;

                }
            }
        }
    }
}

// Returns non-zero if the raster buffer was modified by this update.
static bool32 milton_update(MiltonState* milton_state, MiltonInput* input)
{
    arena_reset(milton_state->transient_arena);
    bool32 updated = 0;
    if (input->scale)
    {
        if (input->scale > 0 && milton_state->view_scale > 2)
        {
            milton_state->view_scale /= 2;
        }
        else if (milton_state->view_scale <= ((int64)1 << 61))
        {
            milton_state->view_scale *= 2;
        }

    }
    // Do a complete re-rasterization.
    if (input->full_refresh || 1)
    {
        uint32* pixels = (uint32_t*)milton_state->raster_buffer;
        for (int y = 0; y < milton_state->screen_size.h; ++y)
        {
            for (int x = 0; x < milton_state->screen_size.w; ++x)
            {
                *pixels++ = 0xff000000;
            }
        }
        updated = 1;
    }
    if (input->brush)
    {
        v2l in_point = *input->brush;

        v2l canvas_point = raster_to_canvas(milton_state, in_point);

        Brush brush = { 0 };
        {
            brush.view_scale = milton_state->view_scale;
            brush.radius = 10;
        }
        // Add to current stroke.

        milton_state->stroke_points[milton_state->num_stroke_points++] = canvas_point;

        rasterize_stroke(milton_state, brush, milton_state->stroke_points,
                milton_state->num_stroke_points);
        updated = 1;
    }
    else if (milton_state->num_stroke_points > 0)
    {
        // Push stroke to history.

        Brush brush = { 0 };
        {
            brush.view_scale = milton_state->view_scale;
            brush.radius = 10;
        }
        Stroke stored;
        stored.brush = brush;
        stored.points = arena_alloc_array(milton_state->root_arena,
                milton_state->num_stroke_points, v2l);
        memcpy(stored.points, milton_state->stroke_points,
                milton_state->num_stroke_points * sizeof(v2l));
        stored.num_points = milton_state->num_stroke_points;

        milton_state->stored_strokes[milton_state->num_stored_strokes++] = stored;

        milton_state->num_stroke_points = 0;
    }
    if (input->reset)
    {
        milton_state->view_scale = 1 << 10;
        milton_state->num_stored_strokes = 0;
        updated = 1;
    }
    // Rasterize *every* stroke...
    for (int i = 0; i < milton_state->num_stored_strokes; ++i)
    {
        Stroke* stored = &milton_state->stored_strokes[i];
        rasterize_stroke(milton_state, stored->brush, stored->points, stored->num_points);
    }

    return updated;
}
