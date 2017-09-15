# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userapp
MODULE_GROUP := usertest

MODULE_SRCS += \
    $(LOCAL_DIR)/vmstress.cpp

MODULE_LIBS := \
    system/ulib/fdio \
    system/ulib/zircon \
    system/ulib/c \

MODULE_STATIC_LIBS := \
    system/ulib/fbl \
    system/ulib/zx \
    system/ulib/zxcpp \
    system/ulib/pretty \
    system/ulib/task-utils

include make/module.mk
