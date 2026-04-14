#
# Makefile for the LCD panel driver.
#

# Each configuration option enables a list of files.
ccflags-$(CONFIG_DRM_MEDIATEK) += -I$(srctree)/drivers/gpu/drm/mediatek/
ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -I$(srctree)/drivers/gpu/drm/mediatek/mediatek_v2/

obj-$(CONFIG_DRM_PANEL_BOE_NT36523N_VDO) += panel-boe-nt36523n-vdo.o
obj-$(CONFIG_DRM_PANEL_CSOT_NT36523N_VDO) += panel-csot-nt36523n-vdo.o

