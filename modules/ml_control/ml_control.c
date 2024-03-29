/*
 * Copyright (C) 2023 Freie Universität Berlin
 * Copyright (C) 2023 Koen Zandberg
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ml_control.h"
#include "ml_control_numbers.h"
#include "net/gcoap.h"
#include "net/coap.h"
#include "uuid.h"
#include "net/nanocoap/nanocbor_helper.h"

#include "nanocbor/nanocbor.h"

#define ENABLE_DEBUG 0
#include "debug.h"

static char _stack[MLCONTROL_STACK_SIZE];

static int _mlcontrol_send_error(nanocbor_encoder_t *enc, mlcontrol_error_t error, const char *description);

static void _submit_fetch(mlcontrol_t *control, const sock_udp_ep_t *remote, uuid_t *uuid, const uint8_t *args, size_t len)
{
    memcpy(&control->fetch.identifier, uuid, sizeof(uuid_t));
    len = MIN(len, MLCONTROL_FETCH_ARG_SIZE);
    memcpy(control->fetch.model_layer_args, args, len);
    memcpy(&control->fetch.remote, remote, sizeof(sock_udp_ep_t));
    control->fetch.args_len = len;
    event_post(&control->queue, &control->fetch_ev);
}

static void _fetch_resp_handler(const gcoap_request_memo_t *memo, coap_pkt_t *pkt, const sock_udp_ep_t *remote)
{
    (void)memo;
    (void)remote;
    mlcontrol_t *control = memo->context;
    if (memo->state == GCOAP_MEMO_RESP) {
        if (pkt->hdr->code != COAP_CODE_CONTENT) {
            printf("Unable to fetch model content: %u\n", coap_get_code(pkt));
            return;
        }

        coap_block1_t block;
        coap_get_block2(pkt, &block);

        if (!block.more) {
            control->fetch.current_block = UINT32_MAX;
        }
        else {
            control->fetch.current_block++;
        }
    }
    event_post(&control->queue, &control->fetch_ev);
}

static void _handle_fetch(event_t *ev)
{
    mlcontrol_t *control = container_of(ev, mlcontrol_t, fetch_ev);
    if (control->fetch.current_block == UINT32_MAX) {
        printf("Fetching model done\n");
        return;
    }
    uint8_t buf[256];
    coap_pkt_t pkt;
    int res = gcoap_req_init(&pkt, buf, sizeof(buf), COAP_METHOD_FETCH, CONFIG_MLCONTROL_MODEL_PATH);
    assert(res == 0);
    coap_opt_add_format(&pkt, COAP_FORMAT_CBOR);

        coap_block_slicer_t slicer;
        coap_block_slicer_init(&slicer, control->fetch.current_block, 64);
        coap_opt_add_block2(&pkt, &slicer, 1);

    ssize_t payload_len = 0;
    size_t len = 0;
    if (control->fetch.current_block == 0) {
        payload_len = coap_opt_finish(&pkt, COAP_OPT_FINISH_PAYLOAD);
        assert(payload_len > 0);

        nanocbor_encoder_t enc;
        nanocbor_encoder_init(&enc, buf + payload_len, sizeof(buf) - payload_len);

        nanocbor_fmt_array(&enc, 2);
        nanocbor_fmt_tag(&enc, 37);
        nanocbor_put_bstr(&enc, (uint8_t*)&control->fetch.identifier, sizeof(uuid_t));
        nanocbor_put_bstr(&enc, control->fetch.model_layer_args, control->fetch.args_len);

        len = nanocbor_encoded_len(&enc);
    }
    else {
        payload_len = coap_opt_finish(&pkt, COAP_OPT_FINISH_NONE);
    }

    gcoap_req_send(buf, payload_len + len, &control->fetch.remote, _fetch_resp_handler, control);
}

static int _handle_rpc_status(mlcontrol_t *control, nanocbor_value_t *arr, nanocbor_encoder_t *encoder)
{
    (void)arr;
    nanocbor_fmt_array(encoder, 2);
    nanocbor_fmt_uint(encoder, 0);
    nanocbor_fmt_bool(encoder, control->training);
    return 0;
}

static int _handle_rpc_fetch_model(mlcontrol_t* control, nanocbor_value_t *args, nanocbor_encoder_t *encoder, const coap_channel_memo_t *memo)
{
    (void)encoder;
    nanocbor_value_t arr;
    int res = nanocbor_enter_array(args, &arr);
    if (res != NANOCBOR_OK) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "Argument array not found");
    }
    uint32_t tag = 0;
    res = nanocbor_get_tag(&arr, &tag);
    if (res != NANOCBOR_OK) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "UUID Tag not found");
    }

    if (tag != 37) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "Incorrect tag in data structure");
    }

    const uint8_t *uuid_bytes = NULL;
    size_t len = 0;

    res = nanocbor_get_bstr(&arr, &uuid_bytes, &len);
    if (res != NANOCBOR_OK) {
        printf("No uuid found\n");
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "UUID byte string not found");
    }

    if (len != sizeof(uuid_t)) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "Invalid UUID length");
    }

    const uint8_t *arguments = NULL;
    size_t arg_len = 0;

    nanocbor_get_subcbor(&arr, &arguments, &arg_len);

    control->training = true;
    _submit_fetch(control, memo->remote, (uuid_t*)uuid_bytes, arguments, arg_len);
    nanocbor_fmt_array(encoder, 1);
    nanocbor_fmt_uint(encoder, 0);
    return 0;
}

static void _submit_upload(mlcontrol_t *control, const sock_udp_ep_t *remote, uuid_t *uuid, const uint8_t *args, size_t len)
{
    memcpy(&control->upload.identifier, uuid, sizeof(uuid_t));
    len = MIN(len, MLCONTROL_FETCH_ARG_SIZE);
    memcpy(control->upload.model_layer_args, args, len);
    memcpy(&control->upload.remote, remote, sizeof(sock_udp_ep_t));
    control->upload.args_len = len;
    control->upload.current_block = 0;
    event_post(&control->queue, &control->upload_ev);
}

static int _handle_rpc_stop_upload_model(mlcontrol_t* control, nanocbor_value_t *args, nanocbor_encoder_t *encoder, const coap_channel_memo_t *memo)
{
    nanocbor_value_t arr;
    puts("Stop and upload");
    int res = nanocbor_enter_array(args, &arr);
    if (res != NANOCBOR_OK) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "Argument array not found");
    }
    uint32_t tag = 0;
    res = nanocbor_get_tag(&arr, &tag);
    if (res != NANOCBOR_OK) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "UUID Tag not found");
    }

    if (tag != 37) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "Incorrect tag in data structure");
    }

    const uint8_t *uuid_bytes = NULL;
    size_t len = 0;

    res = nanocbor_get_bstr(&arr, &uuid_bytes, &len);
    if (res != NANOCBOR_OK) {
        printf("No uuid found\n");
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "UUID byte string not found");
    }

    if (len != sizeof(uuid_t)) {
        return _mlcontrol_send_error(encoder, MLCONTROL_ERR_INVALID_REQ_STRUCTURE, "Invalid UUID length");
    }

    const uint8_t *arguments = NULL;
    size_t arg_len = 0;

    nanocbor_get_subcbor(&arr, &arguments, &arg_len);

    control->upload.current_block = 0;

    control->training = false;
    _submit_upload(control, memo->remote, (uuid_t*)uuid_bytes, arguments, arg_len);

    nanocbor_fmt_array(encoder, 1);
    nanocbor_fmt_uint(encoder, 0);
    return 0;
}

static void _upload_resp_handler(const gcoap_request_memo_t *memo, coap_pkt_t *pdu, const sock_udp_ep_t *remote)
{
    (void)remote;
    mlcontrol_t *control = memo->context;
    unsigned req_state = memo->state;
    if (req_state == GCOAP_MEMO_RESP) {
        if (coap_get_code_raw(pdu) == COAP_CODE_CONTINUE) {
            control->upload.current_block++;
        }
        else {
            control->upload.current_block = UINT32_MAX;
        }
    }
    event_post(&control->queue, &control->upload_ev);
}

static void _fmt_dummy_parameters(nanocbor_encoder_t *enc)
{
    nanocbor_fmt_array(enc, 4);

    nanocbor_fmt_array(enc, 2);
    nanocbor_put_tstr(enc, "w1");
    nanocbor_fmt_array(enc, 8);
    for (size_t i=0; i < 8; i++) {
        nanocbor_fmt_uint(enc, i);
    }

    nanocbor_fmt_array(enc, 2);
    nanocbor_put_tstr(enc, "w2");
    nanocbor_fmt_array(enc, 6);
    for (size_t i=0; i < 6; i++) {
        nanocbor_fmt_uint(enc, i);
    }

    nanocbor_fmt_array(enc, 2);
    nanocbor_put_tstr(enc, "b1");
    nanocbor_fmt_array(enc, 2);
    for (size_t i=0; i < 2; i++) {
        nanocbor_fmt_uint(enc, i);
    }

    nanocbor_fmt_array(enc, 2);
    nanocbor_put_tstr(enc, "b2");
    nanocbor_fmt_array(enc, 3);
    for (size_t i=0; i < 3; i++) {
        nanocbor_fmt_uint(enc, i);
    }
}

static void _fmt_dummy_operators(nanocbor_encoder_t *enc)
{
    nanocbor_fmt_array(enc, 2);

    nanocbor_fmt_array(enc, 2);
    nanocbor_put_tstr(enc, "1");
    nanocbor_fmt_array(enc, 2);
    nanocbor_fmt_uint(enc, 0);
    nanocbor_fmt_uint(enc, 2);

    nanocbor_fmt_array(enc, 2);
    nanocbor_put_tstr(enc, "2");
    nanocbor_fmt_array(enc, 2);
    nanocbor_fmt_uint(enc, 1);
    nanocbor_fmt_uint(enc, 3);
}

static ssize_t _format_upload_payload(mlcontrol_t *control, coap_pkt_t *pdu, coap_block_slicer_t *slicer)
{
    (void)control;
    nanocbor_encoder_t enc;
    coap_nanocbor_slicer_helper_t helper;
    coap_nanocbor_slicer_helper_init(&helper, pdu, slicer);
    coap_nanocbor_encoder_init(&enc, &helper);

    nanocbor_fmt_array(&enc, 4);
    nanocbor_fmt_tag(&enc, 37);
    nanocbor_put_bstr(&enc, (void*)&control->upload.identifier, sizeof(uuid_t));

    _fmt_dummy_parameters(&enc);
    _fmt_dummy_operators(&enc);

    nanocbor_fmt_array(&enc, 2);
    nanocbor_fmt_uint(&enc, 900);
    nanocbor_fmt_uint(&enc, 750);
    return coap_nanocbor_block1_finish(pdu, &helper);
}

static ssize_t _upload_fmt_req(mlcontrol_t *control, uint8_t *buf, size_t buf_len)
{
    coap_pkt_t pdu;
    int res = gcoap_req_init(&pdu, buf, buf_len, COAP_METHOD_POST, CONFIG_MLCONTROL_MODEL_PATH);
    if (res < 0) {
        return res;
    }
    coap_block_slicer_t slicer;

    coap_block_slicer_init(&slicer, control->upload.current_block, 64);
    coap_hdr_set_type(pdu.hdr, COAP_TYPE_CON);
    coap_opt_add_format(&pdu, COAP_FORMAT_CBOR);
    coap_opt_add_block1(&pdu, &slicer, true);
    coap_opt_finish(&pdu, COAP_OPT_FINISH_PAYLOAD);

    return _format_upload_payload(control, &pdu, &slicer);
}

static void _handle_upload(event_t *ev)
{
    mlcontrol_t *control = container_of(ev, mlcontrol_t, upload_ev);
    uint8_t pkt_buf[128];

    if (control->upload.current_block == UINT32_MAX) {
        puts("Done uploading model");
        return;
    }

    ssize_t len = _upload_fmt_req(control, pkt_buf, sizeof(pkt_buf));
    if (len > 0) {
        int res = gcoap_req_send(pkt_buf, len, &control->upload.remote, _upload_resp_handler, control);
        printf("Result from send block %u: %i\n", control->upload.current_block, res);
    }
}

static int _handle_rpc(mlcontrol_t *control, nanocbor_value_t *arr, nanocbor_encoder_t *encoder, uint32_t rpc, const coap_channel_memo_t *memo)
{
    switch (rpc) {
        case MLCONTROL_RPC_STATUS:
            return _handle_rpc_status(control, arr, encoder);
        case MLCONTROL_RPC_START:
            return 0;
        case MLCONTROL_RPC_STOP:
            return 0;
        case MLCONTROL_RPC_FETCH_MODEL:
            return _handle_rpc_fetch_model(control, arr, encoder, memo);
        case MLCONTROL_RPC_STOP_UPLOAD_MODEL:
            return _handle_rpc_stop_upload_model(control, arr, encoder, memo);
        default:
            return -1;
    }
}

static int _channel_callback(coap_channel_t *channel,  void *payload, size_t payload_len, const coap_channel_memo_t *memo)
{
    (void)channel;
    mlcontrol_t *control = memo->ctx;
    assert(control->channel == channel);

    nanocbor_value_t decoder, arr;
    nanocbor_decoder_init(&decoder, memo->payload, memo->payload_len);

    nanocbor_encoder_t encoder;
    nanocbor_encoder_init(&encoder, payload, payload_len);

    int res = nanocbor_enter_array(&decoder, &arr);

    if (res != NANOCBOR_OK) {
        return -1;
    }

    uint32_t rpc_command = 0;
    res = nanocbor_get_uint32(&arr, &rpc_command);

    if (res < NANOCBOR_OK) {
        return -1;
    }
    res = _handle_rpc(control, &arr, &encoder, rpc_command, memo);
    return MIN(nanocbor_encoded_len(&encoder), payload_len);
}


static void *_thread(void *arg) {
    mlcontrol_t *control = arg;

    control->fetch_ev.list_node.next = NULL;
    control->fetch_ev.handler = _handle_fetch;
    control->upload_ev.list_node.next = NULL;
    control->upload_ev.handler = _handle_upload;
    event_queue_init(&control->queue);
    coap_channel_init(control->channel, CONFIG_MLCONTROL_PATH, _channel_callback, control);
    event_loop(&control->queue);
    return NULL;
}

void mlcontrol_init(mlcontrol_t *control, coap_channel_t *channel)
{
    memset(control, 0, sizeof(mlcontrol_t));
    control->channel = channel;
    thread_create(_stack, sizeof(_stack),THREAD_PRIORITY_MAIN - 2, THREAD_CREATE_STACKTEST, _thread,
        control, "mlcontrol");
}

static int _mlcontrol_send_error(nanocbor_encoder_t *enc, mlcontrol_error_t error, const char *description) {
    nanocbor_fmt_array(enc, 2);
    nanocbor_fmt_uint(enc, error);
    nanocbor_put_tstr(enc, description);
    return -1;
}
