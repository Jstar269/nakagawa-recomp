# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
FROM gcr.io/oss-fuzz-base/base-builder:v1
COPY . $SRC/nakagawa-recomp
WORKDIR $SRC/nakagawa-recomp
COPY .clusterfuzzlite/build.sh $SRC/
