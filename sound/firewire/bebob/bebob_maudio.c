/*
 * bebob_maudio.c - a part of driver for BeBoB based devices
 *
 * Copyright (c) 2013 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "./bebob.h"
#include <sound/control.h>

/*
 * Just powering on, Firewire 410/Audiophile/1814 and ProjectMix I/O wait to
 * download firmware blob. To enable these devices, drivers should upload
 * firmware blob and send a command to initialize configuration to factory
 * settings when completing uploading. Then these devices generate bus reset
 * and are recognized as new devices with the firmware.
 *
 * But with firmware version 5058 or later, the firmware is stored to flash
 * memory in the device and drivers can tell DM1000 to load the firmware by
 * sending a cue. This cue must be sent one time.
 *
 * If the firmware blobs are in alsa-firmware package, this driver can support
 * these devices with any firmware versions. (Then this driver need codes to
 * upload the firmware blob.) But for this, the license of firmware blob needs
 * to be considered.
 *
 * For streaming, both of output and input streams are needed for Firewire 410
 * and Ozonic. The single stream is OK for the other devices even if the clock
 * source is not SYT-Match (I note no devices use SYT-Match).
 *
 * Without streaming, the devices except for Firewire Audiophile can mix any
 * input and output. For this reason, Audiophile cannot be used as standalone
 * mixer.
 *
 * Firewire 1814 and ProjectMix I/O uses special firmware. It will be freezed
 * when receiving any commands which the firmware can't understand. These
 * devices utilize completely different system to control. It is some
 * write-transaction directly into a certain address. All of addresses for mixer
 * functionality is between 0xffc700700000 to 0xffc70070009c.
 */

/* Offset from information register */
#define INFO_OFFSET_SW_DATE	0x20

/* Bootloader Protocol Version 1 */
#define MAUDIO_BOOTLOADER_CUE1	0x00000001
/*
 * Initializing configuration to factory settings (= 0x1101), (swapped in line),
 * Command code is zero (= 0x00),
 * the number of operands is zero (= 0x00)(at least significant byte)
 */
#define MAUDIO_BOOTLOADER_CUE2	0x01110000
/* padding */
#define MAUDIO_BOOTLOADER_CUE3	0x00000000

#define MAUDIO_SPECIFIC_ADDRESS	0xffc700000000

#define METER_OFFSET		0x00600000

/* some device has sync info after metering data */
#define METER_SIZE_SPECIAL	84	/* with sync info */
#define METER_SIZE_FW410	76	/* with sync info */
#define METER_SIZE_AUDIOPHILE	60	/* with sync info */
#define METER_SIZE_SOLO		52	/* with sync info */
#define METER_SIZE_OZONIC	48
#define METER_SIZE_NRV10	80

/* labels for metering */
#define ANA_IN		"Analog In"
#define ANA_OUT		"Analog Out"
#define DIG_IN		"Digital In"
#define SPDIF_IN	"S/PDIF In"
#define ADAT_IN		"ADAT In"
#define DIG_OUT		"Digital Out"
#define SPDIF_OUT	"S/PDIF Out"
#define ADAT_OUT	"ADAT Out"
#define STRM_IN		"Stream In"
#define AUX_OUT		"Aux Out"
#define HP_OUT		"HP Out"
/* for NRV */
#define UNKNOWN_METER	"Unknown"

/*
 * For some M-Audio devices, this module just send cue to load firmware. After
 * loading, the device generates bus reset and newly detected.
 *
 * If we make any transactions to load firmware, the operation may failed.
 */
int snd_bebob_maudio_load_firmware(struct fw_unit *unit)
{
	struct fw_device *device = fw_parent_device(unit);
	int err, rcode;
	u64 date;
	__be32 cues[3] = {
		MAUDIO_BOOTLOADER_CUE1,
		MAUDIO_BOOTLOADER_CUE2,
		MAUDIO_BOOTLOADER_CUE3
	};

	/* check date of software used to build */
	err = snd_bebob_read_block(unit, INFO_OFFSET_SW_DATE,
				   &date, sizeof(u64));
	if (err < 0)
		goto end;
	/*
	 * firmware version 5058 or later has date later than "20070401", but
	 * 'date' is not null-terminated.
	 */
	if (date < 0x3230303730343031) {
		dev_err(&unit->device,
			"Use firmware version 5058 or later\n");
		err = -ENOSYS;
		goto end;
	}

	rcode = fw_run_transaction(device->card, TCODE_WRITE_BLOCK_REQUEST,
				   device->node_id, device->generation,
				   device->max_speed, BEBOB_ADDR_REG_REQ,
				   cues, sizeof(cues));
	if (rcode != RCODE_COMPLETE) {
		dev_err(&unit->device,
			"Failed to send a cue to load firmware\n");
		err = -EIO;
	}
end:
	return err;
}

static inline int
get_meter(struct snd_bebob *bebob, void *buf, unsigned int size)
{
	return snd_fw_transaction(bebob->unit, TCODE_READ_BLOCK_REQUEST,
				  MAUDIO_SPECIFIC_ADDRESS + METER_OFFSET,
				  buf, size, 0);
}

static int
check_clk_sync(struct snd_bebob *bebob, unsigned int size, bool *sync)
{
	int err;
	u8 *buf;

	buf = kmalloc(size, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	err = get_meter(bebob, buf, size);
	if (err < 0)
		goto end;

	/* if synced, this value is the same of SFC of FDF in CIP header */
	*sync = (buf[size - 2] != 0xff);
end:
	kfree(buf);
	return err;
}

/*
 * dig_fmt: 0x00:S/PDIF, 0x01:ADAT
 * clk_lock: 0x00:unlock, 0x01:lock
 */
static int
special_clk_set_params(struct snd_bebob *bebob, unsigned int clk_src,
		       unsigned int dig_in_fmt, unsigned int dig_out_fmt,
		       unsigned int clk_lock)
{
	int err;
	u8 *buf;

	if (amdtp_stream_running(&bebob->rx_stream) ||
	    amdtp_stream_running(&bebob->tx_stream))
		return -EBUSY;

	buf = kmalloc(12, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0]  = 0x00;		/* CONTROL */
	buf[1]  = 0xff;		/* UNIT */
	buf[2]  = 0x00;		/* vendor dependent */
	buf[3]  = 0x04;		/* company ID high */
	buf[4]  = 0x00;		/* company ID middle */
	buf[5]  = 0x04;		/* company ID low */
	buf[6]  = 0xff & clk_src;	/* clock source */
	buf[7]  = 0xff & dig_in_fmt;	/* input digital format */
	buf[8]  = 0xff & dig_out_fmt;	/* output digital format */
	buf[9]  = 0xff & clk_lock;	/* lock these settings */
	buf[10] = 0x00;		/* padding  */
	buf[11] = 0x00;		/* padding */

	/* do transaction and check buf[1-9] are the same against command */
	err = fcp_avc_transaction(bebob->unit, buf, 12, buf, 12,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) |
				  BIT(5) | BIT(6) | BIT(7) | BIT(8) |
				  BIT(9));
	if (err < 0)
		goto end;
	if ((err < 6) || (buf[0] != 0x09)) {
		dev_err(&bebob->unit->device,
			"failed to set clock params\n");
		err = -EIO;
		goto end;
	}

	bebob->clk_src		= buf[6];
	bebob->dig_in_fmt	= buf[7];
	bebob->dig_out_fmt	= buf[8];
	bebob->clk_lock		= buf[9];

	snd_ctl_notify(bebob->card, SNDRV_CTL_EVENT_MASK_VALUE,
		       bebob->ctl_id_sync);
end:
	kfree(buf);
	return err;
}
static void
special_stream_formation_set(struct snd_bebob *bebob)
{
	unsigned int i;

	/*
	 * the stream formation is different depending on digital interface
	 */
	if (bebob->dig_in_fmt == 0x01) {
		bebob->tx_stream_formations[3].pcm = 16;
		bebob->tx_stream_formations[4].pcm = 16;
		bebob->tx_stream_formations[5].pcm = 12;
		bebob->tx_stream_formations[6].pcm = 12;
		if (bebob->maudio_is1814) {
			bebob->tx_stream_formations[7].pcm = 2;
			bebob->tx_stream_formations[8].pcm = 2;
		}
	} else {
		bebob->tx_stream_formations[3].pcm = 10;
		bebob->tx_stream_formations[4].pcm = 10;
		bebob->tx_stream_formations[5].pcm = 10;
		bebob->tx_stream_formations[6].pcm = 10;
		if (bebob->maudio_is1814) {
			bebob->tx_stream_formations[7].pcm = 2;
			bebob->tx_stream_formations[8].pcm = 2;
		}
	}

	if (bebob->dig_out_fmt == 0x01) {
		bebob->rx_stream_formations[3].pcm = 12;
		bebob->rx_stream_formations[4].pcm = 12;
		bebob->rx_stream_formations[5].pcm = 8;
		bebob->rx_stream_formations[6].pcm = 8;
		if (bebob->maudio_is1814) {
			bebob->rx_stream_formations[7].pcm = 4;
			bebob->rx_stream_formations[8].pcm = 4;
		}
	} else {
		bebob->rx_stream_formations[3].pcm = 6;
		bebob->rx_stream_formations[4].pcm = 6;
		bebob->rx_stream_formations[5].pcm = 6;
		bebob->rx_stream_formations[6].pcm = 6;
		if (bebob->maudio_is1814) {
			bebob->rx_stream_formations[7].pcm = 4;
			bebob->rx_stream_formations[8].pcm = 4;
		}
	}

	for (i = 3; i < SND_BEBOB_STRM_FMT_ENTRIES; i++) {
		bebob->tx_stream_formations[i].midi = 1;
		bebob->rx_stream_formations[i].midi = 1;
		if ((i > 7) && !bebob->maudio_is1814)
			break;
	}
}

static int snd_bebob_maudio_special_add_controls(struct snd_bebob *bebob);
int
snd_bebob_maudio_special_discover(struct snd_bebob *bebob, bool is1814)
{
	int err;

	bebob->maudio_is1814 = is1814;

	/* initialize these parameters because driver is not allowed to ask */
	err = special_clk_set_params(bebob, 0x03, 0x00, 0x00, 0x00);
	if (err < 0)
		dev_err(&bebob->unit->device,
			"failed to initialize clock params\n");

	err = avc_audio_get_selector(bebob->unit, 0x00, 0x04,
				     &bebob->dig_in_iface);
	if (err < 0)
		dev_err(&bebob->unit->device,
			"failed to get current dig iface.");

	err = snd_bebob_maudio_special_add_controls(bebob);
	if (err < 0)
		return -EIO;

	special_stream_formation_set(bebob);

	if (bebob->maudio_is1814) {
		bebob->midi_input_ports = 1;
		bebob->midi_output_ports = 1;
	} else {
		bebob->midi_input_ports = 2;
		bebob->midi_output_ports = 2;
	}

	bebob->maudio_special_quirk = true;

	return 0;
}

/* Input plug shows actual rate. Output plug is needless for this purpose. */
static int special_get_rate(struct snd_bebob *bebob, unsigned int *rate)
{
	return snd_bebob_get_rate(bebob, rate, AVC_GENERAL_PLUG_DIR_IN);
}
static int special_set_rate(struct snd_bebob *bebob, unsigned int rate)
{
	int err = snd_bebob_set_rate(bebob, rate, AVC_GENERAL_PLUG_DIR_OUT);
	msleep(100);
	if (err < 0)
		goto end;

	err = snd_bebob_set_rate(bebob, rate, AVC_GENERAL_PLUG_DIR_IN);
	msleep(100);
	if (err >= 0)
		snd_ctl_notify(bebob->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       bebob->ctl_id_sync);
end:
	return err;
}

/* Clock source control for special firmware */
static char *special_clk_labels[] = {
	SND_BEBOB_CLOCK_INTERNAL " with Digital Mute", "Digital",
	"Word Clock", SND_BEBOB_CLOCK_INTERNAL};
static int special_clk_get(struct snd_bebob *bebob,
			   unsigned int *id)
{
	*id = bebob->clk_src;
	return 0;
}
static int special_clk_ctl_info(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	einf->count = 1;
	einf->value.enumerated.items = ARRAY_SIZE(special_clk_labels);

	if (einf->value.enumerated.item >= einf->value.enumerated.items)
		einf->value.enumerated.item = einf->value.enumerated.items - 1;

	strcpy(einf->value.enumerated.name,
	       special_clk_labels[einf->value.enumerated.item]);

	return 0;
}
static int special_clk_ctl_get(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);

	spin_lock(&bebob->lock);
	uval->value.enumerated.item[0] = bebob->clk_src;
	spin_unlock(&bebob->lock);

	return 0;
}
static int special_clk_ctl_put(struct snd_kcontrol *kctl,
			       struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	int err, id, changed = 0;

	id = uval->value.enumerated.item[0];
	if (id >= ARRAY_SIZE(special_clk_labels))
		return 0;

	spin_lock(&bebob->lock);
	err = special_clk_set_params(bebob, id,
				     bebob->dig_in_fmt,
				     bebob->dig_out_fmt,
				     bebob->clk_lock);
	changed = (err >= 0);
	spin_unlock(&bebob->lock);

	return changed;
}
static struct snd_kcontrol_new special_clk_ctl = {
	.name	= "Clock Source",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info	= special_clk_ctl_info,
	.get	= special_clk_ctl_get,
	.put	= special_clk_ctl_put
};

/* Clock synchronization control for special firmware */
static int special_sync_ctl_info(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	einf->count = 1;
	einf->value.integer.min = 0;
	einf->value.integer.max = 1;

	return 0;
}
static int special_sync_ctl_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	int err;
	bool synced = 0;

	mutex_lock(&bebob->mutex);
	err = check_clk_sync(bebob, METER_SIZE_SPECIAL, &synced);
	if (err >= 0)
		uval->value.integer.value[0] = synced;
	mutex_unlock(&bebob->mutex);

	return 0;
}
static struct snd_kcontrol_new special_sync_ctl = {
	.name	= "Sync Status",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READ,
	.info	= special_sync_ctl_info,
	.get	= special_sync_ctl_get,
};

/* Digital interface control for special firmware */
static char *special_dig_iface_labels[] = {
	"S/PDIF Optical", "S/PDIF Coaxial", "ADAT Optical"
};
static int special_dig_in_iface_ctl_info(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	einf->count = 1;
	einf->value.enumerated.items = ARRAY_SIZE(special_dig_iface_labels);

	if (einf->value.enumerated.item >= einf->value.enumerated.items)
		einf->value.enumerated.item = einf->value.enumerated.items - 1;

	strcpy(einf->value.enumerated.name,
	       special_dig_iface_labels[einf->value.enumerated.item]);

	return 0;
}
static int special_dig_in_iface_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	int val;

	/* encoded id for user value */
	val = (bebob->dig_in_fmt << 1) | (bebob->dig_in_iface & 0x01);

	/* for ADAT Optical */
	if (val > 2)
		val = 2;

	uval->value.enumerated.item[0] = val;

	return 0;
}
static int special_dig_in_iface_ctl_set(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	unsigned int id, dig_in_fmt, dig_in_iface;
	int err;

	id = uval->value.enumerated.item[0];

	/* decode user value */
	dig_in_fmt = (id >> 1) & 0x01;
	dig_in_iface = id & 0x01;

	err = special_clk_set_params(bebob, bebob->clk_src, dig_in_fmt,
				     bebob->dig_out_fmt, bebob->clk_lock);
	if ((err < 0) || (bebob->dig_in_fmt > 0)) /* ADAT */
		goto end;

	err = avc_audio_set_selector(bebob->unit, 0x00, 0x04, dig_in_iface);
	if (err < 0)
		goto end;

	bebob->dig_in_iface = dig_in_iface;
end:
	special_stream_formation_set(bebob);
	return err;
}
static struct snd_kcontrol_new special_dig_in_iface_ctl = {
	.name	= "Digital Input Interface",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info	= special_dig_in_iface_ctl_info,
	.get	= special_dig_in_iface_ctl_get,
	.put	= special_dig_in_iface_ctl_set
};

static int special_dig_out_iface_ctl_info(struct snd_kcontrol *kctl,
					  struct snd_ctl_elem_info *einf)
{
	einf->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	einf->count = 1;
	einf->value.enumerated.items = ARRAY_SIZE(special_dig_iface_labels) - 1;

	if (einf->value.enumerated.item >= einf->value.enumerated.items)
		einf->value.enumerated.item = einf->value.enumerated.items - 1;

	strcpy(einf->value.enumerated.name,
	       special_dig_iface_labels[einf->value.enumerated.item + 1]);

	return 0;
}
static int special_dig_out_iface_ctl_get(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	uval->value.enumerated.item[0] = bebob->dig_out_fmt;
	return 0;
}
static int special_dig_out_iface_ctl_set(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_value *uval)
{
	struct snd_bebob *bebob = snd_kcontrol_chip(kctl);
	unsigned int id;
	int err;

	id = uval->value.enumerated.item[0];

	err = special_clk_set_params(bebob, bebob->clk_src, bebob->dig_in_fmt,
				     id, bebob->clk_lock);
	if (err >= 0)
		special_stream_formation_set(bebob);

	return err;
}
static struct snd_kcontrol_new special_dig_out_iface_ctl = {
	.name	= "Digital Output Interface",
	.iface	= SNDRV_CTL_ELEM_IFACE_MIXER,
	.access	= SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info	= special_dig_out_iface_ctl_info,
	.get	= special_dig_out_iface_ctl_get,
	.put	= special_dig_out_iface_ctl_set
};

static int snd_bebob_maudio_special_add_controls(struct snd_bebob *bebob)
{
	struct snd_kcontrol *kctl;
	int err;

	kctl = snd_ctl_new1(&special_clk_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
	if (err < 0)
		goto end;

	kctl = snd_ctl_new1(&special_sync_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
	if (err < 0)
		goto end;
	bebob->ctl_id_sync = &kctl->id;

	kctl = snd_ctl_new1(&special_dig_in_iface_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
	if (err < 0)
		goto end;

	kctl = snd_ctl_new1(&special_dig_out_iface_ctl, bebob);
	err = snd_ctl_add(bebob->card, kctl);
end:
	return err;
}

/* Hardware metering for special firmware */
static char *special_meter_labels[] = {
	ANA_IN, ANA_IN, ANA_IN, ANA_IN,
	SPDIF_IN,
	ADAT_IN, ADAT_IN, ADAT_IN, ADAT_IN,
	ANA_OUT, ANA_OUT,
	SPDIF_OUT,
	ADAT_OUT, ADAT_OUT, ADAT_OUT, ADAT_OUT,
	HP_OUT, HP_OUT,
	AUX_OUT
};
static int
special_meter_get(struct snd_bebob *bebob, u32 *target, unsigned int size)
{
	u16 *buf;
	unsigned int i, c, channels;
	int err;

	channels = ARRAY_SIZE(special_meter_labels) * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	/* omit last 4 bytes because it's clock info. */
	buf = kmalloc(METER_SIZE_SPECIAL - 4, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	err = get_meter(bebob, (void *)buf, METER_SIZE_SPECIAL - 4);
	if (err < 0)
		goto end;

	/* some channels are not used, and format is u16 */
	i = 0;
	for (c = 2; c < channels + 2; c++)
		target[i++] = be16_to_cpu(buf[c]) << 16;
end:
	kfree(buf);
	return err;
}

/* Firewire 410 specific operations */
static char *fw410_meter_labels[] = {
	ANA_IN, DIG_IN,
	ANA_OUT, ANA_OUT, ANA_OUT, ANA_OUT, DIG_OUT,
	HP_OUT
};
static int
fw410_meter_get(struct snd_bebob *bebob, u32 *buf, unsigned int size)
{

	unsigned int c, channels;
	int err;

	/* last 4 bytes are omitted because it's clock info. */
	channels = ARRAY_SIZE(fw410_meter_labels) * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	err = get_meter(bebob, (void *)buf, size);
	if (err < 0)
		goto end;

	for (c = 0; c < channels; c++)
		be32_to_cpus(&buf[c]);
end:
	return err;
}

/* Firewire Audiophile specific operation */
static char *audiophile_meter_labels[] = {
	ANA_IN, DIG_IN,
	ANA_OUT, ANA_OUT, DIG_OUT,
	HP_OUT, AUX_OUT,
};
static int
audiophile_meter_get(struct snd_bebob *bebob, u32 *buf, unsigned int size)
{
	unsigned int c, channels;
	int err;

	/* last 4 bytes are omitted because it's clock info. */
	channels = ARRAY_SIZE(audiophile_meter_labels) * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	err = get_meter(bebob, (void *)buf, size);
	if (err < 0)
		goto end;

	for (c = 0; c < channels; c++)
		be32_to_cpus(&buf[c]);
end:
	return err;
}

/* Firewire Solo specific operation */
static char *solo_meter_labels[] = {
	ANA_IN, DIG_IN,
	STRM_IN, STRM_IN,
	ANA_OUT, DIG_OUT
};
static int
solo_meter_get(struct snd_bebob *bebob, u32 *buf, unsigned int size)
{
	unsigned int c;
	int err;
	u32 tmp;

	/* last 4 bytes are omitted because it's clock info. */
	if (size < ARRAY_SIZE(solo_meter_labels) * 2 * sizeof(u32))
		return -EINVAL;

	err = get_meter(bebob, (void *)buf, size);
	if (err < 0)
		goto end;

	c = 0;
	do
		buf[c] = be32_to_cpu(buf[c]);
	while (++c < 4);

	/* swap stream channels because inverted */
	tmp = be32_to_cpu(buf[c]);
	buf[c] = be32_to_cpu(buf[c + 2]);
	buf[c + 2] = tmp;
	tmp = be32_to_cpu(buf[c + 1]);
	buf[c + 1] = be32_to_cpu(buf[c + 3]);
	buf[c + 3] = tmp;

	c += 4;
	do
		be32_to_cpus(&buf[c]);
	while (++c < 12);
end:
	return err;
}

/* Ozonic specific operation */
static char *ozonic_meter_labels[] = {
	ANA_IN, ANA_IN,
	STRM_IN, STRM_IN,
	ANA_OUT, ANA_OUT
};
static int
ozonic_meter_get(struct snd_bebob *bebob, u32 *buf, unsigned int size)
{
	unsigned int c, channels;
	int err;

	channels = ARRAY_SIZE(ozonic_meter_labels) * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	err = get_meter(bebob, (void *)buf, size);
	if (err < 0)
		goto end;

	for (c = 0; c < channels; c++)
		be32_to_cpus(&buf[c]);
end:
	return err;
}

/* NRV10 specific operation */
/* TODO: need testers. these positions are based on my assumption */
static char *nrv10_meter_labels[] = {
	ANA_IN, ANA_IN, ANA_IN, ANA_IN,
	DIG_IN,
	ANA_OUT, ANA_OUT, ANA_OUT, ANA_OUT,
	DIG_IN
};
static int
nrv10_meter_get(struct snd_bebob *bebob, u32 *buf, unsigned int size)
{
	unsigned int c, channels;
	int err;

	channels = ARRAY_SIZE(nrv10_meter_labels) * 2;
	if (size < channels * sizeof(u32))
		return -EINVAL;

	err = get_meter(bebob, (void *)buf, size);
	if (err < 0)
		goto end;

	for (c = 0; c < channels; c++)
		be32_to_cpus(&buf[c]);
end:
	return err;
}

/* for special customized devices */
static struct snd_bebob_rate_spec special_rate_spec = {
	.get	= &special_get_rate,
	.set	= &special_set_rate,
};
static struct snd_bebob_clock_spec special_clk_spec = {
	.num	= ARRAY_SIZE(special_clk_labels),
	.labels	= special_clk_labels,
	.get	= &special_clk_get,
};
static struct snd_bebob_meter_spec special_meter_spec = {
	.num	= ARRAY_SIZE(special_meter_labels),
	.labels	= special_meter_labels,
	.get	= &special_meter_get
};
struct snd_bebob_spec maudio_special_spec = {
	.clock	= &special_clk_spec,
	.rate	= &special_rate_spec,
	.meter	= &special_meter_spec
};

/* Firewire 410 specification */
static struct snd_bebob_rate_spec usual_rate_spec = {
	.get	= &snd_bebob_stream_get_rate,
	.set	= &snd_bebob_stream_set_rate,
};
static struct snd_bebob_meter_spec fw410_meter_spec = {
	.num	= ARRAY_SIZE(fw410_meter_labels),
	.labels	= fw410_meter_labels,
	.get	= &fw410_meter_get
};
struct snd_bebob_spec maudio_fw410_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &fw410_meter_spec
};

/* Firewire Audiophile specification */
static struct snd_bebob_meter_spec audiophile_meter_spec = {
	.num	= ARRAY_SIZE(audiophile_meter_labels),
	.labels	= audiophile_meter_labels,
	.get	= &audiophile_meter_get
};
struct snd_bebob_spec maudio_audiophile_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &audiophile_meter_spec
};

/* Firewire Solo specification */
static struct snd_bebob_meter_spec solo_meter_spec = {
	.num	= ARRAY_SIZE(solo_meter_labels),
	.labels	= solo_meter_labels,
	.get	= &solo_meter_get
};
struct snd_bebob_spec maudio_solo_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &solo_meter_spec
};

/* Ozonic specification */
static struct snd_bebob_meter_spec ozonic_meter_spec = {
	.num	= ARRAY_SIZE(ozonic_meter_labels),
	.labels	= ozonic_meter_labels,
	.get	= &ozonic_meter_get
};
struct snd_bebob_spec maudio_ozonic_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &ozonic_meter_spec
};

/* NRV10 specification */
static struct snd_bebob_meter_spec nrv10_meter_spec = {
	.num	= ARRAY_SIZE(nrv10_meter_labels),
	.labels	= nrv10_meter_labels,
	.get	= &nrv10_meter_get
};
struct snd_bebob_spec maudio_nrv10_spec = {
	.clock	= NULL,
	.rate	= &usual_rate_spec,
	.meter	= &nrv10_meter_spec
};
