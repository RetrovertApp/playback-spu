# SPU/VAG Playback Plugin
#
# Self-contained PlayStation SPU ADPCM decoder. No external library needed.
# Supported formats: VAG (with header), VB (raw ADPCM blocks)

cmake_minimum_required(VERSION 3.16)
project(playback_spu C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

add_library(spu_playback SHARED spu_plugin.c)
rv_set_plugin_properties(spu_playback)
