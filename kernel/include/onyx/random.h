/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
#ifndef _RANDOM_H
#define _RANDOM_H

#include <stddef.h>
#include <stdint.h>

#define ENTROPY_POOL_RANDOM 0
#define ENTROPY_POOL_URANDOM 1

#ifdef __cplusplus
extern "C" {
#endif
void entropy_init_dev(void);
void add_entropy(void *ent, size_t size);
void get_entropy(char *buf, size_t s);
void initialize_entropy();
unsigned int get_random_int(void);

uint32_t arc4random(void);
void arc4random_buf(void* buffer_ptr, size_t size);
uint32_t arc4random_uniform(uint32_t upper_bound);

#ifdef __cplusplus
}
#endif
#endif
