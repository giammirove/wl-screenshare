#ifndef GSR_KMS_SHARED_H
#define GSR_KMS_SHARED_H

#include <stdint.h>
#include <stdbool.h>
#include <drm_mode.h>

#define GSR_KMS_PROTOCOL_VERSION 4

#define GSR_KMS_MAX_ITEMS 8
#define GSR_KMS_MAX_DMA_BUFS 4

typedef struct gsr_kms_response_dma_buf gsr_kms_response_dma_buf;
typedef struct gsr_kms_response_item gsr_kms_response_item;
typedef struct gsr_kms_response gsr_kms_response;

typedef enum {
    KMS_REQUEST_TYPE_REPLACE_CONNECTION,
    KMS_REQUEST_TYPE_GET_KMS
} gsr_kms_request_type;

typedef enum {
    KMS_RESULT_OK,
    KMS_RESULT_INVALID_REQUEST,
    KMS_RESULT_FAILED_TO_GET_PLANE,
    KMS_RESULT_FAILED_TO_GET_PLANES,
    KMS_RESULT_FAILED_TO_SEND
} gsr_kms_result;

typedef struct {
    uint32_t version; /* GSR_KMS_PROTOCOL_VERSION */
    int type;         /* gsr_kms_request_type */
    int new_connection_fd;
} gsr_kms_request;

struct gsr_kms_response_dma_buf {
    int fd;
    uint32_t pitch;
    uint32_t offset;
};

struct gsr_kms_response_item {
    gsr_kms_response_dma_buf dma_buf[GSR_KMS_MAX_DMA_BUFS];
    int num_dma_bufs;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint64_t modifier;
    uint32_t connector_id; /* 0 if unknown */
    bool is_cursor;
    bool has_hdr_metadata;
    int x;
    int y;
    int src_w;
    int src_h;
    struct hdr_output_metadata hdr_metadata;
};

struct gsr_kms_response {
    uint32_t version; /* GSR_KMS_PROTOCOL_VERSION */
    int result;       /* gsr_kms_result */
    char err_msg[128];
    gsr_kms_response_item items[GSR_KMS_MAX_ITEMS];
    int num_items;
};

#endif /* #define GSR_KMS_SHARED_H */
