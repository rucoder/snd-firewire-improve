/*
 * fireworks_command.c - driver for Firewire devices from Echo Digital Audio
 *
 * Copyright (c) 2013 Takashi Sakamoto <o-takashi@sakmocchi.jp>
 *
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.
 *
 * This driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this driver; if not, see <http://www.gnu.org/licenses/>.
 *
 * mostly based on FFADO's souce, which is
 * Copyright (C) 2005-2008 by Pieter Palmers
 *
 */

#include "../lib.h"
#include "./fireworks.h"


/*
 * Echo's Fireworks(TM) utilize its own command.
 * This module calls it as 'Echo Fireworks Commands' (a.k.a EFC).
 *
 * EFC substance:
 *  At first, 6 data exist. we call these data as 'EFC fields'.
 *  Following to the 6 data, parameters for each commands exists.
 *  Most of parameters are 32 bit. But exception exists according to command.
 *   data[0]:	Length of EFC substance.
 *   data[1]:	EFC version
 *   data[2]:	Sequence number. This is incremented by both host and target
 *   data[3]:	EFC category
 *   data[4]:	EFC command
 *   data[5]:	EFC return value in EFC response.
 *   data[6-]:	parameters
 *
 * EFC address:
 *  command:	0xecc000000000
 *  response:	0xecc080000000
 *
 * As a result, Echo's Fireworks doesn't need AVC generic command sets.
 */

struct efc_fields {
	u32 length;
	u32 version;
	u32 seqnum;
	u32 category;
	u32 command;
	u32 retval;
	u32 params[0];
};
#define EFC_HEADER_QUADLETS 6

/* for clock source and sampling rate */
struct efc_clock {
	u32 source;
	u32 sampling_rate;
	u32 index;
};

/* command categories */
enum efc_category {
	EFC_CAT_HWINFO			= 0,
	EFC_CAT_FLASH			= 1,
	EFC_CAT_TRANSPORT		= 2,
	EFC_CAT_HWCTL			= 3,
	EFC_CAT_MIXER_PHYS_OUT		= 4,
	EFC_CAT_MIXER_PHYS_IN		= 5,
	EFC_CAT_MIXER_PLAYBACK		= 6,
	EFC_CAT_MIXER_CAPTURE		= 7,
	EFC_CAT_MIXER_MONITOR		= 8,
	EFC_CAT_IOCONF			= 9,
};

/* hardware info category commands */
enum efc_cmd_hwinfo {
	EFC_CMD_HWINFO_GET_CAPS			= 0,
	EFC_CMD_HWINFO_GET_POLLED		= 1,
	EFC_CMD_HWINFO_SET_EFR_ADDRESS		= 2,
	EFC_CMD_HWINFO_READ_SESSION_BLOCK	= 3,
	EFC_CMD_HWINFO_GET_DEBUG_INFO		= 4,
	EFC_CMD_HWINFO_SET_DEBUG_TRACKING	= 5
};

/* flash category commands */
enum efc_cmd_flash {
	EFC_CMD_FLASH_ERASE		= 0,
	EFC_CMD_FLASH_READ		= 1,
	EFC_CMD_FLASH_WRITE		= 2,
	EFC_CMD_FLASH_GET_STATUS	= 3,
	EFC_CMD_FLASH_GET_SESSION_BASE	= 4,
	EFC_CMD_FLASH_LOCK		= 5
};

/* hardware control category commands */
enum efc_cmd_hwctl {
	EFC_CMD_HWCTL_SET_CLOCK		= 0,
	EFC_CMD_HWCTL_GET_CLOCK		= 1,
	EFC_CMD_HWCTL_BSX_HANDSHAKE	= 2,
	EFC_CMD_HWCTL_CHANGE_FLAGS	= 3,
	EFC_CMD_HWCTL_GET_FLAGS		= 4,
	EFC_CMD_HWCTL_IDENTIFY		= 5,
	EFC_CMD_HWCTL_RECONNECT_PHY	= 6
};
/* for flags */
#define EFC_HWCTL_FLAG_MIXER_UNUSABLE	0x00
#define EFC_HWCTL_FLAG_MIXER_USABLE	0x01
#define EFC_HWCTL_FLAG_DIGITAL_PRO	0x02
#define EFC_HWCTL_FLAG_DIGITAL_RAW	0x04

/* I/O config category commands */
enum efc_cmd_ioconf {
	EFC_CMD_IOCONF_SET_MIRROR	= 0,
	EFC_CMD_IOCONF_GET_MIRROR	= 1,
	EFC_CMD_IOCONF_SET_DIGITAL_MODE	= 2,
	EFC_CMD_IOCONF_GET_DIGITAL_MODE	= 3,
	EFC_CMD_IOCONF_SET_PHANTOM	= 4,
	EFC_CMD_IOCONF_GET_PHANTOM	= 5,
	EFC_CMD_IOCONF_SET_ISOC_MAP	= 6,
	EFC_CMD_IOCONF_GET_ISOC_MAP	= 7,
};

/* return values in response */
enum efc_retval {
	EFC_RETVAL_OK			= 0,
	EFC_RETVAL_BAD			= 1,
	EFC_RETVAL_BAD_COMMAND		= 2,
	EFC_RETVAL_COMM_ERR		= 3,
	EFC_RETVAL_BAD_QUAD_COUNT	= 4,
	EFC_RETVAL_UNSUPPORTED		= 5,
	EFC_RETVAL_1394_TIMEOUT		= 6,
	EFC_RETVAL_DSP_TIMEOUT		= 7,
	EFC_RETVAL_BAD_RATE		= 8,
	EFC_RETVAL_BAD_CLOCK		= 9,
	EFC_RETVAL_BAD_CHANNEL		= 10,
	EFC_RETVAL_BAD_PAN		= 11,
	EFC_RETVAL_FLASH_BUSY		= 12,
	EFC_RETVAL_BAD_MIRROR		= 13,
	EFC_RETVAL_BAD_LED		= 14,
	EFC_RETVAL_BAD_PARAMETER	= 15,
	EFC_RETVAL_INCOMPLETE		= 0x80000000
};

/* for phys_in/phys_out/playback/capture/monitor category commands */
enum snd_efw_mixer_cmd {
	SND_EFW_MIXER_SET_GAIN		= 0,
	SND_EFW_MIXER_GET_GAIN		= 1,
	SND_EFW_MIXER_SET_MUTE		= 2,
	SND_EFW_MIXER_GET_MUTE		= 3,
	SND_EFW_MIXER_SET_SOLO		= 4,
	SND_EFW_MIXER_GET_SOLO		= 5,
	SND_EFW_MIXER_SET_PAN		= 6,
	SND_EFW_MIXER_GET_PAN		= 7,
	SND_EFW_MIXER_SET_NOMINAL	= 8,
	SND_EFW_MIXER_GET_NOMINAL	= 9
};

static int
efc_transaction_run(struct fw_unit *unit,
		    const void *command, unsigned int command_size,
		    void *response, unsigned int response_size,
		    unsigned int response_match_bytes);

static int
efc(struct snd_efw *efw, unsigned int category,
		unsigned int command,
		const u32 *params, unsigned int param_count,
		void *response, unsigned int response_quadlets)
{
	int err;

	unsigned int cmdbuf_bytes;
	__be32 *cmdbuf;
	struct efc_fields *efc_fields;
	u32 sequence_number;
	unsigned int i;

	/* calculate buffer size*/
	cmdbuf_bytes = EFC_HEADER_QUADLETS * 4
			 + max(param_count, response_quadlets) * 4;

	/* keep buffer */
	cmdbuf = kzalloc(cmdbuf_bytes, GFP_KERNEL);
	if (cmdbuf == NULL)
		return -ENOMEM;

	/* fill efc fields */
	efc_fields		= (struct efc_fields *)cmdbuf;
	efc_fields->length	= EFC_HEADER_QUADLETS + param_count;
	efc_fields->version	= 1;
	efc_fields->category	= category;
	efc_fields->command	= command;
	efc_fields->retval	= 0;

	/* sequence number should keep consistency */
	spin_lock(&efw->lock);
	efc_fields->seqnum = efw->sequence_number++;
	sequence_number = efw->sequence_number;
	spin_unlock(&efw->lock);

	/* fill EFC parameters */
	for (i = 0; i < param_count; i++)
		efc_fields->params[i] = params[i];

	/* for endian-ness*/
	for (i = 0; i < (cmdbuf_bytes / 4); i++)
		cmdbuf[i] = cpu_to_be32(cmdbuf[i]);

	/* if return value is positive, it means return bytes */
	/* TODO: the last parameter should be sequence number */
	err = efc_transaction_run(efw->unit, cmdbuf, cmdbuf_bytes,
				  cmdbuf, cmdbuf_bytes, 0);
	if (err < 0)
		goto end;

	/* for endian-ness */
	for (i = 0; i < (err / 4); i += 1)
		cmdbuf[i] = be32_to_cpu(cmdbuf[i]);

	/* check EFC response fields */
	if ((efc_fields->seqnum != sequence_number) ||
	    (efc_fields->version < 1) ||
	    (efc_fields->category != category) ||
	    (efc_fields->command != command) ||
	    (efc_fields->retval != EFC_RETVAL_OK)) {
		dev_err(&efw->unit->device, "EFC failed [%u/%u]: %X\n",
			efc_fields->category, efc_fields->command,
			efc_fields->retval);
		err = -EIO;
		goto end;
	}

	/* fill response buffer */
	if (response != NULL) {
		memset(response, 0, response_quadlets * 4);
		response_quadlets = min(response_quadlets, efc_fields->length);
		memcpy(response, efc_fields->params, response_quadlets * 4);
	}

	err = 0;

end:
	kfree(cmdbuf);
	return err;
}

int snd_efw_command_identify(struct snd_efw *efw)
{
	return efc(efw, EFC_CAT_HWCTL,
				EFC_CMD_HWCTL_IDENTIFY,
				NULL, 0, NULL, 0);
}

int snd_efw_command_get_hwinfo(struct snd_efw *efw,
			       struct snd_efw_hwinfo *hwinfo)
{
	u32 *tmp;
	int i;
	int count;
	int err = efc(efw, EFC_CAT_HWINFO,
				EFC_CMD_HWINFO_GET_CAPS,
				NULL, 0, hwinfo, sizeof(*hwinfo) / 4);
	if (err < 0)
		goto end;

	/* arrangement for endianness */
	count = HWINFO_NAME_SIZE_BYTES / 4;
	tmp = (u32 *)&hwinfo->vendor_name;
	for (i = 0; i < count; i += 1)
		tmp[i] = cpu_to_be32(tmp[i]);
	tmp = (u32 *)&hwinfo->model_name;
	for (i = 0; i < count; i += 1)
		tmp[i] = cpu_to_be32(tmp[i]);

	count = sizeof(struct snd_efw_phys_group) * HWINFO_MAX_CAPS_GROUPS / 4;
	tmp = (u32 *)&hwinfo->out_groups;
	for (i = 0; i < count; i += 1)
		tmp[i] = cpu_to_be32(tmp[i]);
	tmp = (u32 *)&hwinfo->in_groups;
	for (i = 0; i < count; i += 1)
		tmp[i] = cpu_to_be32(tmp[i]);

	/* ensure terminated */
	hwinfo->vendor_name[HWINFO_NAME_SIZE_BYTES - 1] = '\0';
	hwinfo->model_name[HWINFO_NAME_SIZE_BYTES  - 1] = '\0';

	err = 0;
end:
	return err;
}

int snd_efw_command_get_phys_meters(struct snd_efw *efw,
				    struct snd_efw_phys_meters *meters,
				    int len)
{
	return efc(efw, EFC_CAT_HWINFO,
				EFC_CMD_HWINFO_GET_POLLED,
				NULL, 0, meters, len / 4);
}

static int
command_get_clock(struct snd_efw *efw, struct efc_clock *clock)
{
	return efc(efw, EFC_CAT_HWCTL,
				EFC_CMD_HWCTL_GET_CLOCK,
				NULL, 0, clock, sizeof(struct efc_clock) / 4);
}

static int
command_set_clock(struct snd_efw *efw,
			  int source, int sampling_rate)
{
	int err;

	struct efc_clock clock = {0};

	/* check arguments */
	if ((source < 0) && (sampling_rate < 0)) {
		err = -EINVAL;
		goto end;
	}

	/* get current status */
	err = command_get_clock(efw, &clock);
	if (err < 0)
		goto end;

	/* no need */
	if ((clock.source == source) &&
	    (clock.sampling_rate == sampling_rate))
		goto end;

	/* set params */
	if ((source >= 0) && (clock.source != source))
		clock.source = source;
	if ((sampling_rate > 0) && (clock.sampling_rate != sampling_rate))
		clock.sampling_rate = sampling_rate;
	clock.index = 0;

	err = efc(efw, EFC_CAT_HWCTL,
				EFC_CMD_HWCTL_SET_CLOCK,
				(u32 *)&clock, 3, NULL, 0);

	err = 0;
end:
	return err;
}

int snd_efw_command_get_clock_source(struct snd_efw *efw,
				     enum snd_efw_clock_source *source)
{
	int err;
	struct efc_clock clock = {0};

	err = command_get_clock(efw, &clock);
	if (err >= 0)
		*source = clock.source;

	return err;
}

int snd_efw_command_set_clock_source(struct snd_efw *efw,
				     enum snd_efw_clock_source source)
{
	return command_set_clock(efw, source, -1);
}

int snd_efw_command_get_sampling_rate(struct snd_efw *efw,
				      int *sampling_rate)
{
	int err;
	struct efc_clock clock = {0};

	err = command_get_clock(efw, &clock);
	if (err >= 0)
		*sampling_rate = clock.sampling_rate;

	return err;
}

int
snd_efw_command_set_sampling_rate(struct snd_efw *efw, int sampling_rate)
{
	return command_set_clock(efw, -1, sampling_rate);
}

int snd_efw_command_get_iec60958_format(struct snd_efw *efw,
					enum snd_efw_iec60958_format *format)
{
	int err;
	u32 flag = {0};

	err = efc(efw, EFC_CAT_HWCTL,
				EFC_CMD_HWCTL_GET_FLAGS,
				NULL, 0, &flag, 1);
	if (err >= 0) {
		if (flag & EFC_HWCTL_FLAG_DIGITAL_PRO)
			*format = SND_EFW_IEC60958_FORMAT_PROFESSIONAL;
		else
			*format = SND_EFW_IEC60958_FORMAT_CONSUMER;
	}

	return err;
}

int snd_efw_command_set_iec60958_format(struct snd_efw *efw,
					enum snd_efw_iec60958_format format)
{
	/*
	 * mask[0]: for set
	 * mask[1]: for clear
	 */
	u32 mask[2] = {0};

	if (format == SND_EFW_IEC60958_FORMAT_PROFESSIONAL)
		mask[0] = EFC_HWCTL_FLAG_DIGITAL_PRO;
	else
		mask[1] = EFC_HWCTL_FLAG_DIGITAL_PRO;

	return efc(efw, EFC_CAT_HWCTL,
				EFC_CMD_HWCTL_CHANGE_FLAGS,
				(u32 *)mask, 2, NULL, 0);
}

int snd_efw_command_get_digital_interface(struct snd_efw *efw,
			enum snd_efw_digital_interface *digital_interface)
{
	int err;
	u32 value = 0;

	err = efc(efw, EFC_CAT_IOCONF,
				EFC_CMD_IOCONF_GET_DIGITAL_MODE,
				NULL, 0, &value, 1);

	if (err >= 0)
		*digital_interface = value;

	return err;
}

int snd_efw_command_set_digital_interface(struct snd_efw *efw,
			enum snd_efw_digital_interface digital_interface)
{
	u32 value = digital_interface;

	return efc(efw, EFC_CAT_IOCONF,
				EFC_CMD_IOCONF_SET_DIGITAL_MODE,
				&value, 1, NULL, 0);
}

#define INITIAL_MEMORY_SPACE_EFC_COMMAND	0xecc000000000
#define INITIAL_MEMORY_SPACE_EFC_RESPONSE	0xecc080000000
/* this for juju convinience */
#define INITIAL_MEMORY_SPACE_EFC_END		0xecc080000200

#define ERROR_RETRIES 3
#define ERROR_DELAY_MS 5
#define EFC_TIMEOUT_MS 125

static DEFINE_SPINLOCK(transactions_lock);
static LIST_HEAD(transactions);

enum efc_state {
	STATE_PENDING,
	STATE_BUS_RESET,
	STATE_COMPLETE
};

struct efc_transaction {
	struct list_head list;
	struct fw_unit *unit;
	void *response_buffer;
	unsigned int response_size;
	unsigned int response_match_bytes;
	enum efc_state state;
	wait_queue_head_t wait;
};

static int
efc_transaction_run(struct fw_unit *unit,
		    const void *command, unsigned int command_size,
		    void *response, unsigned int response_size,
		    unsigned int response_match_bytes)
{
	struct efc_transaction t;
	int tcode, ret, tries = 0;

	t.unit = unit;
	t.response_buffer = response;
	t.response_size = response_size;
	t.response_match_bytes = response_match_bytes;
	t.state = STATE_PENDING;
	init_waitqueue_head(&t.wait);

	spin_lock_irq(&transactions_lock);
	list_add_tail(&t.list, &transactions);
	spin_unlock_irq(&transactions_lock);

	do {
		tcode = command_size == 4 ? TCODE_WRITE_QUADLET_REQUEST
					  : TCODE_WRITE_BLOCK_REQUEST;
		ret = snd_fw_transaction(t.unit, tcode,
					 INITIAL_MEMORY_SPACE_EFC_COMMAND,
					 (void *)command, command_size);
		if (ret < 0)
			break;

		wait_event_timeout(t.wait, t.state != STATE_PENDING,
				   msecs_to_jiffies(EFC_TIMEOUT_MS));

		if (t.state == STATE_COMPLETE) {
			ret = t.response_size;
			break;
		} else if (t.state == STATE_BUS_RESET) {
			msleep(ERROR_DELAY_MS);
		} else if (++tries >= ERROR_RETRIES) {
			dev_err(&t.unit->device, "EFC command timed out\n");
			ret = -EIO;
			break;
		}
	} while(1);

	spin_lock_irq(&transactions_lock);
	list_del(&t.list);
	spin_unlock_irq(&transactions_lock);

	return ret;
}

static bool
is_matching_response(struct efc_transaction *transaction,
		     const void *response, size_t length)
{
	const u8 *p1, *p2;
	unsigned int mask, i;

	p1 = response;
	p2 = transaction->response_buffer;
	mask = transaction->response_match_bytes;

	for (i = 0; ; i++) {
		if ((mask & 1) && (p1[i] != p2[i]))
			return false;
		mask >>= 1;
		if (!mask)
			return true;
		if (--length == 0)
			return false;
	}
}

static void
efc_response(struct fw_card *card, struct fw_request *request,
	     int tcode, int destination, int source,
	     int generation, unsigned long long offset,
	     void *data, size_t length, void *callback_data)
{
	struct efc_transaction *t;
	unsigned long flags;

	if ((length < 1) || (*(const u8*)data & 0xf0) != EFC_RETVAL_OK)
		return;

	spin_lock_irqsave(&transactions_lock, flags);
	list_for_each_entry(t, &transactions, list) {
		struct fw_device *device = fw_parent_device(t->unit);
		if ((device->card != card) ||
		    (device->generation != generation))
			continue;
		smp_rmb();	/* node_id vs. generation */
		if (device->node_id != source)
			continue;

		if ((t->state == STATE_PENDING) &&
		    is_matching_response(t, data, length)) {
			t->state = STATE_COMPLETE;
			t->response_size = min((unsigned int)length,
					       t->response_size);
			memcpy(t->response_buffer, data, t->response_size);
			wake_up(&t->wait);
		}
	}
	spin_unlock_irqrestore(&transactions_lock, flags);
}

void snd_efw_command_bus_reset(struct fw_unit *unit)
{
	struct efc_transaction *t;

	spin_lock_irq(&transactions_lock);
	list_for_each_entry(t, &transactions, list) {
		if ((t->unit == unit) &&
		    (t->state == STATE_PENDING)) {
			t->state = STATE_BUS_RESET;
			wake_up(&t->wait);
		}
	}
	spin_unlock_irq(&transactions_lock);
}

static struct fw_address_handler response_register_handler = {
	/* TODO: this span should be reconsidered */
	.length = 0x200,
	.address_callback = efc_response
};

int snd_efw_command_create(void)
{
	static const struct fw_address_region response_register_region = {
		.start	= INITIAL_MEMORY_SPACE_EFC_RESPONSE,
		.end	= INITIAL_MEMORY_SPACE_EFC_END
	};

	return fw_core_add_address_handler(&response_register_handler,
					   &response_register_region);
}

void snd_efw_command_destroy(void)
{
	WARN_ON(!list_empty(&transactions));
	fw_core_remove_address_handler(&response_register_handler);
}
