#include "../include/overclock.h"
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// HACK!!!: When a program uses cuda (including nvenc) then the nvidia driver drops to max performance level - 1 (memory transfer rate is dropped and possibly graphics clock).
// Nvidia does this because in some very extreme cases of cuda there can be memory corruption when running at max memory transfer rate.
// So to get around this we overclock memory transfer rate (maybe this should also be done for graphics clock?) to the best performance level while GPU Screen Recorder is running.

static int min_int(int a, int b) {
    return a < b ? a : b;
}

// Fields are 0 if not set
typedef struct {
    int perf;

    int nv_clock;
    int nv_clock_min;
    int nv_clock_max;

    int mem_clock;
    int mem_clock_min;
    int mem_clock_max;

    int mem_transfer_rate;
    int mem_transfer_rate_min;
    int mem_transfer_rate_max;
} NVCTRLPerformanceLevel;

#define MAX_PERFORMANCE_LEVELS 12
typedef struct {
    NVCTRLPerformanceLevel performance_level[MAX_PERFORMANCE_LEVELS];
    int num_performance_levels;
} NVCTRLPerformanceLevelQuery;

typedef void (*split_callback)(const char *str, size_t size, void *userdata);
static void split_by_delimiter(const char *str, size_t size, char delimiter, split_callback callback, void *userdata) {
    const char *it = str;
    while(it < str + size) {
        const char *prev_it = it;
        it = memchr(it, delimiter, (str + size) - it);
        if(!it)
            it = str + size;

        callback(prev_it, it - prev_it, userdata);
        it += 1; // skip delimiter
    }
}

typedef enum {
    NVCTRL_GPU_NVCLOCK,
    NVCTRL_ATTRIB_GPU_MEM_TRANSFER_RATE,
} NvCTRLAttributeType;

static unsigned int attribute_type_to_attribute_param(NvCTRLAttributeType attribute_type) {
    switch(attribute_type) {
        case NVCTRL_GPU_NVCLOCK:
            return NV_CTRL_GPU_NVCLOCK_OFFSET;
        case NVCTRL_ATTRIB_GPU_MEM_TRANSFER_RATE:
            return NV_CTRL_GPU_MEM_TRANSFER_RATE_OFFSET;
    }
    return 0;
}

static unsigned int attribute_type_to_attribute_param_all_levels(NvCTRLAttributeType attribute_type) {
    switch(attribute_type) {
        case NVCTRL_GPU_NVCLOCK:
            return NV_CTRL_GPU_NVCLOCK_OFFSET_ALL_PERFORMANCE_LEVELS;
        case NVCTRL_ATTRIB_GPU_MEM_TRANSFER_RATE:
            return NV_CTRL_GPU_MEM_TRANSFER_RATE_OFFSET_ALL_PERFORMANCE_LEVELS;
    }
    return 0;
}

// Returns 0 on error
static int xnvctrl_get_attribute_max_value(gsr_xnvctrl *xnvctrl, int num_performance_levels, NvCTRLAttributeType attribute_type) {
    NVCTRLAttributeValidValuesRec valid;
    if(xnvctrl->XNVCTRLQueryValidTargetAttributeValues(xnvctrl->display, NV_CTRL_TARGET_TYPE_GPU, 0, 0, attribute_type_to_attribute_param_all_levels(attribute_type), &valid)) {
        return valid.u.range.max;
    }

    if(num_performance_levels > 0 && xnvctrl->XNVCTRLQueryValidTargetAttributeValues(xnvctrl->display, NV_CTRL_TARGET_TYPE_GPU, 0, num_performance_levels - 1, attribute_type_to_attribute_param(attribute_type), &valid)) {
        return valid.u.range.max;
    }
    
    return 0;
}

static bool xnvctrl_set_attribute_offset(gsr_xnvctrl *xnvctrl, int num_performance_levels, int offset, NvCTRLAttributeType attribute_type) {
    bool success = false;

    // NV_CTRL_GPU_MEM_TRANSFER_RATE_OFFSET_ALL_PERFORMANCE_LEVELS works (or at least used to?) without Xorg running as root
    // so we try that first. NV_CTRL_GPU_MEM_TRANSFER_RATE_OFFSET_ALL_PERFORMANCE_LEVELS also only works with GTX 1000+.
    // TODO: Reverse engineer NVIDIA Xorg driver so we can set this always without root access.
    if(xnvctrl->XNVCTRLSetTargetAttributeAndGetStatus(xnvctrl->display, NV_CTRL_TARGET_TYPE_GPU, 0, 0, attribute_type_to_attribute_param_all_levels(attribute_type), offset))
        success = true;

    for(int i = 0; i < num_performance_levels; ++i) {
        success |= xnvctrl->XNVCTRLSetTargetAttributeAndGetStatus(xnvctrl->display, NV_CTRL_TARGET_TYPE_GPU, 0, i, attribute_type_to_attribute_param(attribute_type), offset);
    }

    return success;
}

static void strip(const char **str, int *size) {
    const char *str_d = *str;
    int s_d = *size;

    const char *start = str_d;
    const char *end = start + s_d;

    while(str_d < end) {
        char c = *str_d;
        if(c != ' ' && c != '\t' && c != '\n')
            break;
        ++str_d;
    }

    int start_offset = str_d - start;
    while(s_d > start_offset) {
        char c = start[s_d];
        if(c != ' ' && c != '\t' && c != '\n')
            break;
        --s_d;
    }

    *str = str_d;
    *size = s_d;
}

static void attribute_callback(const char *str, size_t size, void *userdata) {
    if(size > 255 - 1)
        return;

    int size_i = size;
    strip(&str, &size_i);

    char attribute[255];
    memcpy(attribute, str, size_i);
    attribute[size_i] = '\0';

    const char *sep = strchr(attribute, '=');
    if(!sep)
        return;

    const char *attribute_name = attribute;
    size_t attribute_name_len = sep - attribute_name;
    const char *attribute_value_str = sep + 1;

    int attribute_value = 0;
    if(sscanf(attribute_value_str, "%d", &attribute_value) != 1)
        return;

    NVCTRLPerformanceLevel *performance_level = userdata;
    if(attribute_name_len == 4 && memcmp(attribute_name, "perf", 4) == 0)
        performance_level->perf = attribute_value;
    else if(attribute_name_len == 7 && memcmp(attribute_name, "nvclock", 7) == 0)
        performance_level->nv_clock = attribute_value;
    else if(attribute_name_len == 10 && memcmp(attribute_name, "nvclockmin", 10) == 0)
        performance_level->nv_clock_min = attribute_value;
    else if(attribute_name_len == 10 && memcmp(attribute_name, "nvclockmax", 10) == 0)
        performance_level->nv_clock_max = attribute_value;
    else if(attribute_name_len == 8 && memcmp(attribute_name, "memclock", 8) == 0)
        performance_level->mem_clock = attribute_value;
    else if(attribute_name_len == 11 && memcmp(attribute_name, "memclockmin", 11) == 0)
        performance_level->mem_clock_min = attribute_value;
    else if(attribute_name_len == 11 && memcmp(attribute_name, "memclockmax", 11) == 0)
        performance_level->mem_clock_max = attribute_value;
    else if(attribute_name_len == 15 && memcmp(attribute_name, "memTransferRate", 15) == 0)
        performance_level->mem_transfer_rate = attribute_value;
    else if(attribute_name_len == 18 && memcmp(attribute_name, "memTransferRatemin", 18) == 0)
        performance_level->mem_transfer_rate_min = attribute_value;
    else if(attribute_name_len == 18 && memcmp(attribute_name, "memTransferRatemax", 18) == 0)
        performance_level->mem_transfer_rate_max = attribute_value;
}

static void attribute_line_callback(const char *str, size_t size, void *userdata) {
    NVCTRLPerformanceLevelQuery *query = userdata;
    if(query->num_performance_levels >= MAX_PERFORMANCE_LEVELS)
        return;

    NVCTRLPerformanceLevel *current_performance_level = &query->performance_level[query->num_performance_levels];
    memset(current_performance_level, 0, sizeof(NVCTRLPerformanceLevel));
    ++query->num_performance_levels;
    split_by_delimiter(str, size, ',', attribute_callback, current_performance_level);
}

static bool xnvctrl_get_performance_levels(gsr_xnvctrl *xnvctrl, NVCTRLPerformanceLevelQuery *query) {
    bool success = false;
    memset(query, 0, sizeof(NVCTRLPerformanceLevelQuery));

    char *attributes = NULL;
    if(!xnvctrl->XNVCTRLQueryTargetStringAttribute(xnvctrl->display, NV_CTRL_TARGET_TYPE_GPU, 0, 0, NV_CTRL_STRING_PERFORMANCE_MODES, &attributes)) {
        success = false;
        goto done;
    }

    split_by_delimiter(attributes, strlen(attributes), ';', attribute_line_callback, query);
    success = true;

    done:
    if(attributes)
        XFree(attributes);

    return success;
}

static int compare_mem_transfer_rate_max_asc(const void *a, const void *b) {
    const NVCTRLPerformanceLevel *perf_a = a;
    const NVCTRLPerformanceLevel *perf_b = b;
    return perf_a->mem_transfer_rate_max - perf_b->mem_transfer_rate_max;
}

bool gsr_overclock_load(gsr_overclock *self, Display *display) {
    memset(self, 0, sizeof(gsr_overclock));
    self->num_performance_levels = 0;

    return gsr_xnvctrl_load(&self->xnvctrl, display);
}

void gsr_overclock_unload(gsr_overclock *self) {
    gsr_xnvctrl_unload(&self->xnvctrl);
}

bool gsr_overclock_start(gsr_overclock *self) {
    int basep = 0;
    int errorp = 0;
    if(!self->xnvctrl.XNVCTRLQueryExtension(self->xnvctrl.display, &basep, &errorp)) {
        fprintf(stderr, "gsr warning: gsr_overclock_start: xnvctrl is not supported on your system, failed to overclock memory transfer rate\n");
        return false;
    }

    NVCTRLPerformanceLevelQuery query;
    if(!xnvctrl_get_performance_levels(&self->xnvctrl, &query) || query.num_performance_levels == 0) {
        fprintf(stderr, "gsr warning: gsr_overclock_start: failed to get performance levels for overclocking\n");
        return false;
    }
    self->num_performance_levels = query.num_performance_levels;

    qsort(query.performance_level, query.num_performance_levels, sizeof(NVCTRLPerformanceLevel), compare_mem_transfer_rate_max_asc);

    int target_transfer_rate_offset = xnvctrl_get_attribute_max_value(&self->xnvctrl, query.num_performance_levels, NVCTRL_ATTRIB_GPU_MEM_TRANSFER_RATE);
    if(query.num_performance_levels > 1) {
        const int transfer_rate_max_diff = query.performance_level[query.num_performance_levels - 1].mem_transfer_rate_max - query.performance_level[query.num_performance_levels - 2].mem_transfer_rate_max;
        target_transfer_rate_offset = min_int(target_transfer_rate_offset, transfer_rate_max_diff);
        if(target_transfer_rate_offset >= 0 && xnvctrl_set_attribute_offset(&self->xnvctrl, self->num_performance_levels, target_transfer_rate_offset, NVCTRL_ATTRIB_GPU_MEM_TRANSFER_RATE)) {
            fprintf(stderr, "gsr info: gsr_overclock_start: sucessfully set memory transfer rate offset to %d\n", target_transfer_rate_offset);
        } else {
            fprintf(stderr, "gsr info: gsr_overclock_start: failed to overclock memory transfer rate offset to %d\n", target_transfer_rate_offset);
        }
    }

    // TODO: Sort by nv_clock_max

    // TODO: Enable. Crashes on my system (gtx 1080) so it's disabled for now. Seems to crash even if graphics clock is increasd by 1, let alone 1200
    /*
    int target_nv_clock_offset = xnvctrl_get_attribute_max_value(&self->xnvctrl, query.num_performance_levels, NVCTRL_GPU_NVCLOCK);
    if(query.num_performance_levels > 1) {
        const int nv_clock_max_diff = query.performance_level[query.num_performance_levels - 1].nv_clock_max - query.performance_level[query.num_performance_levels - 2].nv_clock_max;
        target_nv_clock_offset = min_int(target_nv_clock_offset, nv_clock_max_diff);
        if(target_nv_clock_offset >= 0 && xnvctrl_set_attribute_offset(&self->xnvctrl, self->num_performance_levels, target_nv_clock_offset, NVCTRL_GPU_NVCLOCK)) {
            fprintf(stderr, "gsr info: gsr_overclock_start: sucessfully set nv clock offset to %d\n", target_nv_clock_offset);
        } else {
            fprintf(stderr, "gsr info: gsr_overclock_start: failed to overclock nv clock offset to %d\n", target_nv_clock_offset);
        }
    }
    */

    XSync(self->xnvctrl.display, False);
    return true;
}

void gsr_overclock_stop(gsr_overclock *self) {
    xnvctrl_set_attribute_offset(&self->xnvctrl, self->num_performance_levels, 0, NVCTRL_ATTRIB_GPU_MEM_TRANSFER_RATE);
    //xnvctrl_set_attribute_offset(&self->xnvctrl, self->num_performance_levels, 0, NVCTRL_GPU_NVCLOCK);
    XSync(self->xnvctrl.display, False);
}
