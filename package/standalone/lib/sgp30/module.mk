INCLUDE += -I$(SGP30_BASE)
VPATH   += $(SGP30_BASE)
DEFINE	+= -DSUPPORT_SGP30

CSRCS += \
	sgp30.c