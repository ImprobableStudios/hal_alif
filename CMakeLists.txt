# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_SOC_ENSEMBLE_RTSS alif_ensemble_cmsis_dfp)
zephyr_include_directories(zephyr/include)
