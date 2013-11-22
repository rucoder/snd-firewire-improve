/*
 * bebob_command.c - driver for BeBoB based devices
 *
 * Copyright (c) 2013 Takashi Sakamoto
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
 */

#include "./bebob.h"

#define BEBOB_COMMAND_MAX_TRIAL	3
#define BEBOB_COMMAND_WAIT_MSEC	100

int avc_audio_set_selector(struct fw_unit *unit, unsigned int subunit_id,
			   unsigned int fb_id, unsigned int num)
{
	u8 *buf;
	int err;

	buf = kzalloc(12, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0]  = 0x00;		/* AV/C CONTROL */
	buf[1]  = 0x08 | (0x07 & subunit_id);	/* AUDIO SUBUNIT ID */
	buf[2]  = 0xb8;		/* FUNCTION BLOCK  */
	buf[3]  = 0x80;		/* type is 'selector'*/
	buf[4]  = 0xff & fb_id;	/* function block id */
	buf[5]  = 0x10;		/* control attribute is CURRENT */
	buf[6]  = 0x02;		/* selector length is 2 */
	buf[7]  = 0xff & num;	/* input function block plug number */
	buf[8]  = 0x01;		/* control selector is SELECTOR_CONTROL */

	/* do transaction and check buf[1-8] are the same against command */
	err = fcp_avc_transaction(unit, buf, 12, buf, 12,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) |
				  BIT(6) | BIT(7) | BIT(8));
	if (err < 0)
		goto end;
	if ((err < 6) || (buf[0] != 0x09)) {
		dev_err(&unit->device,
			"failed to set selector %d: 0x%02X\n",
			fb_id, buf[0]);
		err = -EIO;
		goto end;
	}

	err = 0;
end:
	kfree(buf);
	return err;
}

int avc_audio_get_selector(struct fw_unit *unit, unsigned int subunit_id,
			   unsigned int fb_id, unsigned int *num)
{
	u8 *buf;
	int err;

	buf = kzalloc(12, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0]  = 0x01;		/* AV/C STATUS */
	buf[1]  = 0x08 | (0x07 & subunit_id);	/* AUDIO SUBUNIT ID */
	buf[2]  = 0xb8;		/* FUNCTION BLOCK */
	buf[3]  = 0x80;		/* type is 'selector'*/
	buf[4]  = 0xff & fb_id;	/* function block id */
	buf[5]  = 0x10;		/* control attribute is CURRENT */
	buf[6]  = 0x02;		/* selector length is 2 */
	buf[7]  = 0xff;		/* input function block plug number */
	buf[8]  = 0x01;		/* control selector is SELECTOR_CONTROL */

	/* do transaction and check buf[1-6,8] are the same against command */
	err = fcp_avc_transaction(unit, buf, 12, buf, 12,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) |
				  BIT(6) | BIT(8));
	if (err < 0)
		goto end;
	if ((err < 6) || (buf[0] != 0x0c)) {
		dev_err(&unit->device,
			"failed to get selector %d: 0x%02X\n",
			fb_id, buf[0]);
		err = -EIO;
		goto end;
	}

	*num = buf[7];
	err = 0;
end:
	kfree(buf);
	return err;
}

int avc_ccm_get_sig_src(struct fw_unit *unit,
	unsigned int *src_stype, unsigned int *src_sid, unsigned int *src_pid,
	unsigned int dst_stype, unsigned int dst_sid, unsigned int dst_pid)
{
	int err;
	u8 *buf;

	buf = kzalloc(8, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0] = 0x01;	/* AV/C STATUS */
	buf[1] = 0xff;	/* UNIT */
	buf[2] = 0x1A;	/* SIGNAL SOURCE */
	buf[3] = 0x0f;
	buf[4] = 0xff;
	buf[5] = 0xfe;
	buf[6] = (0xf8 & (dst_stype << 3)) | dst_sid;
	buf[7] = 0xff & dst_pid;

	/* do transaction and check buf[1,2,6,7] are the same against command */
	err = fcp_avc_transaction(unit, buf, 8, buf, 8,
				  BIT(1) | BIT(2) | BIT(6) | BIT(7));
	if (err < 0)
		goto end;
	if ((err < 0) || (buf[0] != 0x0c)) {
		dev_err(&unit->device,
			"failed to get signal status\n");
		err = -EIO;
		goto end;
	}

	*src_stype = buf[4] >> 3;
	*src_sid = buf[4] & 0x07;
	*src_pid = buf[5];
end:
	kfree(buf);
	return err;
}

int avc_ccm_set_sig_src(struct fw_unit *unit,
	unsigned int src_stype, unsigned int src_sid, unsigned int src_pid,
	unsigned int dst_stype, unsigned int dst_sid, unsigned int dst_pid)
{
	int err;
	u8 *buf;

	buf = kzalloc(8, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0] = 0x00;	/* AV/C CONTROL */
	buf[1] = 0xff;	/* UNIT */
	buf[2] = 0x1A;	/* SIGNAL SOURCE */
	buf[3] = 0x0f;
	buf[4] = (0xf8 & (src_stype << 3)) | src_sid;
	buf[5] = 0xff & src_pid;
	buf[6] = (0xf8 & (dst_stype << 3)) | dst_sid;
	buf[7] = 0xff & dst_pid;

	/* do transaction and check buf[1-7] are the same against command */
	err = fcp_avc_transaction(unit, buf, 8, buf, 8,
				  BIT(1) | BIT(2) | BIT(4) | BIT(5) |
				  BIT(6) | BIT(7));
	if (err < 0)
		goto end;
	if ((err < 0) || ((buf[0] != 0x09) && (buf[0] != 0x0f))) {
		dev_err(&unit->device,
			"failed to set signal status\n");
		err = -EIO;
		goto end;
	}

	err = 0;
end:
	kfree(buf);
	return err;
}

int avc_bridgeco_get_plug_type(struct fw_unit *unit,
			       enum snd_bebob_plug_dir pdir,
			       enum snd_bebob_plug_unit punit,
			       unsigned short pid,
			       enum snd_bebob_plug_type *type)
{
	u8 *buf;
	int err;

	buf = kzalloc(12, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0] = 0x01;		/* AV/C STATUS */
	buf[1] = 0xff;		/* UNIT */
	buf[2] = 0x02;		/* PLUG INFO */
	buf[3] = 0xC0;		/* Extended Plug Info */
	buf[4] = pdir;		/* plug direction */
	buf[5] = 0x00;		/* address mode [0x00/0x01/0x02] */
	buf[6] = punit;		/* plug unit type */
	buf[7]  = 0xff & pid;	/* plug id */
	buf[8]  = 0xff;		/* reserved */
	buf[9]  = 0x00;		/* info type is 'plug type' */
	buf[10] = 0xff;		/* plug type in response */

	/* do transaction and check buf[1-7,9] are the same against command */
	err = fcp_avc_transaction(unit, buf, 12, buf, 12,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) |
				  BIT(6) | BIT(7) | BIT(9));
	if (err < 0)
		goto end;
	/* IMPLEMENTED/STABLE is OK */
	else if ((err < 6) || (buf[0] != 0x0c)) {
		err = -EIO;
		goto end;
	}

	*type = buf[10];
	err = 0;

end:
	kfree(buf);
	return err;
}

int avc_bridgeco_get_plug_ch_pos(struct fw_unit *unit,
				 enum snd_bebob_plug_dir pdir,
				 unsigned short pid, u8 *buf, unsigned int len)
{
	unsigned int trial;
	int err;

	/* check given buffer */
	if ((buf == NULL) || (len < 256)) {
		err = -EINVAL;
		goto end;
	}

	buf[0] = 0x01;		/* AV/C STATUS */
	buf[1] = 0xff;		/* Unit */
	buf[2] = 0x02;		/* PLUG INFO */
	buf[3] = 0xC0;		/* Extended Plug Info */
	buf[4] = pdir;		/* plug direction */
	buf[5] = 0x00;		/* address mode is 'Unit' */
	buf[6] = 0x00;		/* plug unit type is 'ISOC'*/
	buf[7] = 0xff & pid;	/* plug id */
	buf[8] = 0xff;		/* reserved */
	buf[9] = 0x03;		/* info type is 'channel position' */

	/*
	 * NOTE:
	 * M-Audio Firewire 410 returns 0x09 (ACCEPTED) just after changing
	 * signal format even if this command asks STATE. This is not in
	 * AV/C command specification.
	 */
	for (trial = 0; trial < BEBOB_COMMAND_MAX_TRIAL; trial++) {
		/* do transaction and check buf[1-7,9] are the same */
		err = fcp_avc_transaction(unit, buf, 12, buf, 256,
					  BIT(1) | BIT(2) | BIT(3) | BIT(4) |
					  BIT(5) | BIT(6) | BIT(7) | BIT(9));
		if (err < 0)
			goto end;
		else if (err < 6) {
			err = -EIO;
			goto end;
		} else if (buf[0] == 0x0c)
			break;
		else if (trial < BEBOB_COMMAND_MAX_TRIAL)
			msleep(BEBOB_COMMAND_WAIT_MSEC);
		else {
			err = -EIO;
			goto end;
		}
	}

	/* strip command header */
	memmove(buf, buf + 10, err - 10);
	err = 0;
end:
	return err;
}

int avc_bridgeco_get_plug_cluster_type(struct fw_unit *unit,
				       enum snd_bebob_plug_dir pdir,
				       unsigned int pid, unsigned int cluster_id,
				       u8 *type)
{
	u8 *buf;
	int err;

	/* cluster info includes characters but this module don't need it */
	buf = kzalloc(12, GFP_KERNEL);
	if (buf == NULL)
		return -ENOMEM;

	buf[0] = 0x01;		/* AV/C STATUS */
	buf[1] = 0xff;		/* UNIT */
	buf[2] = 0x02;		/* PLUG INFO */
	buf[3] = 0xc0;		/* Extended Plug Info */
	buf[4] = pdir;		/* plug direction */
	buf[5] = 0x00;		/* address mode is 'Unit' */
	buf[6] = 0x00;		/* plug unit type is 'ISOC' */
	buf[7] = 0xff & pid;	/* plug id */
	buf[8] = 0xff;		/* reserved */
	buf[9] = 0x07;		/* info type is 'cluster info' */
	buf[10] = 0xff & (cluster_id + 1);	/* cluster id */
	buf[11] = 0x00;		/* type in response */

	/* do transaction and check buf[1-7,9,10] are the same */
	err = fcp_avc_transaction(unit, buf, 12, buf, 12,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) |
				  BIT(6) | BIT(7) | BIT(9) | BIT(10));
	if (err < 0)
		goto end;
	else if ((err < 12) && (buf[0] != 0x0c)) {
		err = -EIO;
		goto end;
	}

	*type = buf[11];
	err = 0;
end:
	kfree(buf);
	return err;
}

int avc_bridgeco_get_plug_strm_fmt(struct fw_unit *unit,
				   enum snd_bebob_plug_dir pdir,
				   unsigned short pid,
				   unsigned int entryid, u8 *buf,
				   unsigned int *len)
{
	int err;

	/* check given buffer */
	if ((buf == NULL) || (*len < 12)) {
		err = -EINVAL;
		goto end;
	}

	/* fill buffer as command */
	buf[0] = 0x01;			/* AV/C STATUS */
	buf[1] = 0xff;			/* unit */
	buf[2] = 0x2f;			/* opcode is STREAM FORMAT SUPPORT */
	buf[3] = 0xc1;			/* COMMAND LIST, BridgeCo extension */
	buf[4] = pdir;			/* plug direction */
	buf[5] = 0x00;			/* address mode is 'Unit' */
	buf[6] = 0x00;			/* plug unit type is 'ISOC' */
	buf[7] = 0xff & pid;		/* plug ID */
	buf[8] = 0xff;			/* reserved */
	buf[9] = 0xff;			/* stream status in response */
	buf[10] = 0xff & entryid;	/* entry ID */

	/* do transaction and check buf[1-7,10] are the same against command */
	err = fcp_avc_transaction(unit, buf, 12, buf, *len,
				  BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(5) |
				  BIT(6) | BIT(7) | BIT(10));
	if (err < 0)
		goto end;
	/* reach the end of entries */
	else if (buf[0] == 0x0a) {
		err = 0;
		*len = 0;
		goto end;
	} else if (buf[0] != 0x0c) {
		err = -EINVAL;
		goto end;
	/* the header of this command is 11 bytes */
	} else if (err < 12) {
		err = -EIO;
		goto end;
	} else if (buf[10] != entryid) {
		err = -EIO;
		goto end;
	}

	/* strip command header */
	memmove(buf, buf + 11, err - 11);
	*len = err - 11;
	err = 0;
end:
	return err;
}

int snd_bebob_get_rate(struct snd_bebob *bebob, unsigned int *rate,
		       enum avc_general_plug_dir dir)
{
	int err;

	err = avc_general_get_sig_fmt(bebob->unit, rate, dir, 0);
	if (err < 0)
		goto end;

	/* IMPLEMENTED/STABLE is OK */
	if (err != 0x0c) {
		dev_err(&bebob->unit->device,
			"failed to get sampling rate\n");
		err = -EIO;
	}
end:
	return err;
}

int snd_bebob_set_rate(struct snd_bebob *bebob, unsigned int rate,
		       enum avc_general_plug_dir dir)
{
	int err;

	err = avc_general_set_sig_fmt(bebob->unit, rate, dir, 0);
	if (err < 0)
		goto end;

	/* ACCEPTED or INTERIM is OK */
	if ((err != 0x0f) && (err != 0x09)) {
		dev_err(&bebob->unit->device,
			"failed to set sampling rate\n");
		err = -EIO;
	}
end:
	return err;
}