/*
 * test_stratum_msg.c — unit tests for the Stratum message parser/emitters.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "stratum_msg.h"

static void test_parse_notify(void){
    const char *l = "{\"id\":null,\"method\":\"mining.notify\",\"params\":"
        "[\"6a68\",\"prevhash\",\"cb1\",\"cb2\",[],\"20000000\",\"1a2b\",\"64\",true]}";
    stratum_msg_t m; assert(stratum_msg_parse(l,&m)==0);
    assert(m.type==SM_NOTIFY);
    assert(strcmp(m.job_id,"6a68")==0);
    assert(m.clean_jobs==true);
}
static void test_parse_submit(void){
    const char *l = "{\"id\":4,\"method\":\"mining.submit\",\"params\":"
        "[\"wallet.w1\",\"6a68\",\"0000\",\"64\",\"deadbeef\",\"00c8\"]}";
    stratum_msg_t m; assert(stratum_msg_parse(l,&m)==0);
    assert(m.type==SM_SUBMIT && m.id==4);
    assert(strcmp(m.job_id,"6a68")==0);
    assert(strcmp(m.worker,"wallet.w1")==0);
}

static void test_emit_set_extranonce(void){
    char buf[256];
    int n = sm_emit_set_extranonce(buf, sizeof buf, "29cc886a", 8);
    assert(n > 0);
    assert(strcmp(buf, "{\"id\":null,\"method\":\"mining.set_extranonce\",\"params\":[\"29cc886a\",8]}")==0);
    stratum_msg_t m; assert(stratum_msg_parse(buf,&m)==0);
    assert(m.type==SM_SET_EXTRANONCE);
    assert(strcmp(m.enonce1,"29cc886a")==0);
    assert(m.n2len==8);
}

static void test_emit_set_difficulty(void){
    char buf[256];
    int n = sm_emit_set_difficulty(buf, sizeof buf, 1000.0);
    assert(n > 0);
    stratum_msg_t m; assert(stratum_msg_parse(buf,&m)==0);
    assert(m.type==SM_SET_DIFFICULTY);
    assert(m.diff==1000.0);
}

static void test_emit_overflow(void){
    char buf[8];
    int n = sm_emit_set_extranonce(buf, sizeof buf, "29cc886a", 8);
    assert(n == -1);
}

int main(void){
    test_parse_notify();
    test_parse_submit();
    test_emit_set_extranonce();
    test_emit_set_difficulty();
    test_emit_overflow();
    printf("stratum_msg: parse notify+submit, emit round-trips passed\n");
    return 0;
}
