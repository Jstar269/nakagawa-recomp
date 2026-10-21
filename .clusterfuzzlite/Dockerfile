# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
FROM gcr.io/oss-fuzz-base/base-builder:v1@sha256:1f9cf74bc3d7029b528494542f9d9851b256dceed38d3f420989434dc5ae0760
COPY . $SRC/nakagawa-recomp
WORKDIR $SRC/nakagawa-recomp
COPY .clusterfuzzlite/build.sh $SRC/
