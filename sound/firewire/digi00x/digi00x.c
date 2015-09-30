/*
 * digi00x.c - a part of driver for Digidesign Digi 002/003 family
 *
 * Copyright (c) 2014-2015 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "digi00x.h"

MODULE_DESCRIPTION("Digidesign Digi 002/003 family Driver");
MODULE_AUTHOR("Takashi Sakamoto <o-takashi@sakamocchi.jp>");
MODULE_LICENSE("GPL v2");

#define VENDOR_DIGIDESIGN	0x00a07e
#define MODEL_DIGI00X		0x000002

static int name_card(struct snd_dg00x *dg00x)
{
	struct fw_device *fw_dev = fw_parent_device(dg00x->unit);
	char name[32] = {0};
	char *model;
	int err;

	err = fw_csr_string(dg00x->unit->directory, CSR_MODEL, name,
			    sizeof(name));
	if (err < 0)
		return err;

	model = skip_spaces(name);

	strcpy(dg00x->card->driver, "Digi00x");
	strcpy(dg00x->card->shortname, model);
	strcpy(dg00x->card->mixername, model);
	snprintf(dg00x->card->longname, sizeof(dg00x->card->longname),
		 "Digidesign %s, GUID %08x%08x at %s, S%d", model,
		 cpu_to_be32(fw_dev->config_rom[3]),
		 cpu_to_be32(fw_dev->config_rom[4]),
		 dev_name(&dg00x->unit->device), 100 << fw_dev->max_speed);

	return 0;
}

static void dg00x_card_free(struct snd_card *card)
{
	struct snd_dg00x *dg00x = card->private_data;

	fw_unit_put(dg00x->unit);

	mutex_destroy(&dg00x->mutex);
}

static int snd_dg00x_probe(struct fw_unit *unit,
			   const struct ieee1394_device_id *entry)
{
	struct snd_card *card;
	struct snd_dg00x *dg00x;
	int err;

	/* create card */
	err = snd_card_new(&unit->device, -1, NULL, THIS_MODULE,
			   sizeof(struct snd_dg00x), &card);
	if (err < 0)
		return err;
	card->private_free = dg00x_card_free;

	/* initialize myself */
	dg00x = card->private_data;
	dg00x->card = card;
	dg00x->unit = fw_unit_get(unit);

	mutex_init(&dg00x->mutex);

	err = name_card(dg00x);
	if (err < 0)
		goto error;

	err = snd_card_register(card);
	if (err < 0)
		goto error;

	dev_set_drvdata(&unit->device, dg00x);

	return err;
error:
	snd_card_free(card);
	return err;
}

static void snd_dg00x_update(struct fw_unit *unit)
{
	return;
}

static void snd_dg00x_remove(struct fw_unit *unit)
{
	struct snd_dg00x *dg00x = dev_get_drvdata(&unit->device);

	/* No need to wait for releasing card object in this context. */
	snd_card_free_when_closed(dg00x->card);
}

static const struct ieee1394_device_id snd_dg00x_id_table[] = {
	/* Both of 002/003 use the same ID. */
	{
		.match_flags = IEEE1394_MATCH_VENDOR_ID |
			       IEEE1394_MATCH_MODEL_ID,
		.vendor_id = VENDOR_DIGIDESIGN,
		.model_id = MODEL_DIGI00X,
	},
	{}
};
MODULE_DEVICE_TABLE(ieee1394, snd_dg00x_id_table);

static struct fw_driver dg00x_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "snd-firewire-digi00x",
		.bus = &fw_bus_type,
	},
	.probe    = snd_dg00x_probe,
	.update   = snd_dg00x_update,
	.remove   = snd_dg00x_remove,
	.id_table = snd_dg00x_id_table,
};

static int __init snd_dg00x_init(void)
{
	return driver_register(&dg00x_driver.driver);
}

static void __exit snd_dg00x_exit(void)
{
	driver_unregister(&dg00x_driver.driver);
}

module_init(snd_dg00x_init);
module_exit(snd_dg00x_exit);