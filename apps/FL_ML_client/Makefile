# Default Makefile, for host native GNRC-based networking

# name of your application
APPLICATION = FL_ML_client

# If no BOARD is found in the environment, use this default:
BOARD ?= native

# This has to be the absolute path to the TinyPART repo base directory:
TINYPARTBASE ?= $(CURDIR)/../..

# Include packages that pull up and auto-init the link layer.
# NOTE: 6LoWPAN will be included if IEEE802.15.4 devices are present
USEMODULE += netdev_default

USEMODULE += auto_init_gnrc_netif
# Specify the mandatory networking modules
USEMODULE += gnrc_ipv6_default
# Additional networking modules that can be dropped if not needed
USEMODULE += gnrc_icmpv6_echo
USEMODULE += gcoap

USEMODULE += fmt
USEMODULE += netutils
USEMODULE += random

# Add also the shell, some shell commands
USEMODULE += shell
USEMODULE += shell_cmds_default
USEMODULE += ps
USEMODULE += nanocoap_nanocbor_helper
USEPKG += nanocbor

USEMODULE += coap_channel
USEMODULE += cord_endpoint_singleton
USEMODULE += cord_endpoint
USEMODULE += ml_control

# Comment this out to disable code in RIOT that does safety checking
# which is not needed in a production environment but helps in the
# development process:
DEVELHELP ?= 1

# Change this to 0 show compiler invocation lines by default:
QUIET ?= 1

include $(TINYPARTBASE)/Makefile.include

# Client ID of this instance
FL_CLIENT_ID ?= 1

## Uncomment to redefine port, for example use 61616 for RFC 6282 UDP compression.
#GCOAP_PORT = 5683
#CFLAGS += -DCONFIG_GCOAP_PORT=$(GCOAP_PORT)

## Uncomment to redefine request token length, max 8.
#GCOAP_TOKENLEN = 2
#CFLAGS += -DCONFIG_GCOAP_TOKENLEN=$(GCOAP_TOKENLEN)

CORD_ENDPOINT_SINGLETON_ADDRESS ?= \"[2001:db8::481:f3b1:4556:7d42]\"

# Increase from default for confirmable block2 follow-on requests
GCOAP_RESEND_BUFS_MAX ?= 4
CFLAGS += -DCONFIG_GCOAP_RESEND_BUFS_MAX=$(GCOAP_RESEND_BUFS_MAX)
CFLAGS += -DCORD_ENDPOINT_SINGLETON_ADDRESS=$(CORD_ENDPOINT_SINGLETON_ADDRESS)

CFLAGS += -DFL_CLIENT_ID=$(FL_CLIENT_ID)
