#pragma once

#include <sys/types.h>

struct kvm_segment {
        uint64_t base;
        uint32_t limit;
        uint16_t selector;
        uint8_t  type;
        uint8_t  present, dpl, db, s, l, g, avl;
        uint8_t  unusable;
        uint8_t  padding;
};
