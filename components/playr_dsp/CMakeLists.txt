idf_component_register(SRCS dsp_core.c dsp_config.c playr_dsp.c
						INCLUDE_DIRS include
						PRIV_REQUIRES json console heap platform_config
)

# the project builds for size (-Os); the filter loops want speed
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
