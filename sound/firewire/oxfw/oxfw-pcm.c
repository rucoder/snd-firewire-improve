/*
 * oxfw_pcm.c - a part of driver for OXFW970/971 based devices
 *
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de>
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "oxfw.h"

static int hw_rule_rate(struct snd_pcm_hw_params *params,
			struct snd_pcm_hw_rule *rule)
{
	struct snd_oxfw_stream_formation *formations = rule->private;
	struct snd_interval *r =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	const struct snd_interval *c =
		hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_interval t = {
		.min = UINT_MAX, .max = 0, .integer = 1
	};
	unsigned int i;

	for (i = 0; i < SND_OXFW_STREAM_FORMAT_ENTRIES; i++) {
		if (formations[i].rate == 0)
			continue;

		if (!snd_interval_test(c, formations[i].pcm))
			continue;

		t.min = min(t.min, formations[i].rate);
		t.max = max(t.max, formations[i].rate);

	}
	return snd_interval_refine(r, &t);
}

static int hw_rule_channels(struct snd_pcm_hw_params *params,
			    struct snd_pcm_hw_rule *rule)
{
	struct snd_oxfw_stream_formation *formations = rule->private;
	struct snd_interval *c =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	const struct snd_interval *r =
		hw_param_interval_c(params, SNDRV_PCM_HW_PARAM_RATE);
	unsigned int i, j, count, list[SND_OXFW_STREAM_FORMAT_ENTRIES] = {0};

	count = 0;
	for (i = 0; i < SND_OXFW_STREAM_FORMAT_ENTRIES; i++) {
		if (formations[i].rate == 0)
			continue;

		if (!snd_interval_test(r, formations[i].rate))
			continue;
		if (list[count] == formations[i].pcm)
			continue;

		for (j = 0; j < ARRAY_SIZE(list); j++) {
			if (list[j] == formations[i].pcm)
				break;
		}
		if (j == ARRAY_SIZE(list)) {
			list[count] = formations[i].pcm;
			if (++count == ARRAY_SIZE(list))
				break;
		}
	}

	return snd_interval_list(c, count, list, 0);
}

static void limit_channels_and_rates(struct snd_pcm_hardware *hw,
				struct snd_oxfw_stream_formation *formations)
{
	unsigned int i;

	hw->channels_min = UINT_MAX;
	hw->channels_max = 0;

	hw->rate_min = UINT_MAX;
	hw->rate_max = 0;
	hw->rates = 0;

	for (i = 0; i < SND_OXFW_STREAM_FORMAT_ENTRIES; i++) {
		if (formations[i].rate == 0)
			continue;

		hw->channels_min = min(hw->channels_min, formations[i].pcm);
		hw->channels_max = max(hw->channels_max, formations[i].pcm);

		hw->rate_min = min(hw->rate_min, formations[i].rate);
		hw->rate_max = max(hw->rate_max, formations[i].rate);
		hw->rates |= snd_pcm_rate_to_rate_bit(formations[i].rate);
	}
}

static void limit_period_and_buffer(struct snd_pcm_hardware *hw)
{
	hw->periods_min = 2;		/* SNDRV_PCM_INFO_BATCH */
	hw->periods_max = UINT_MAX;

	hw->period_bytes_min = 4 * hw->channels_max;	/* bytes for a frame */

	/* Just to prevent from allocating much pages. */
	hw->period_bytes_max = hw->period_bytes_min * 2048;
	hw->buffer_bytes_max = hw->period_bytes_max * hw->periods_min;
}

static int init_hw_params(struct snd_oxfw *oxfw,
			  struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_oxfw_stream_formation *formations;
	struct amdtp_stream *stream;
	int err;

	runtime->hw.info = SNDRV_PCM_INFO_BATCH |
			   SNDRV_PCM_INFO_BLOCK_TRANSFER |
			   SNDRV_PCM_INFO_INTERLEAVED |
			   SNDRV_PCM_INFO_JOINT_DUPLEX |
			   SNDRV_PCM_INFO_MMAP |
			   SNDRV_PCM_INFO_MMAP_VALID;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		runtime->hw.formats = AMDTP_IN_PCM_FORMAT_BITS;
		stream = &oxfw->tx_stream;
		formations = oxfw->tx_stream_formations;
	} else {
		runtime->hw.formats = AMDTP_OUT_PCM_FORMAT_BITS;
		stream = &oxfw->rx_stream;
		formations = oxfw->rx_stream_formations;
	}

	limit_channels_and_rates(&runtime->hw, formations);
	limit_period_and_buffer(&runtime->hw);

	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_CHANNELS,
				  hw_rule_channels, formations,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (err < 0)
		goto end;

	err = snd_pcm_hw_rule_add(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
				  hw_rule_rate, formations,
				  SNDRV_PCM_HW_PARAM_CHANNELS, -1);
	if (err < 0)
		goto end;

	err = amdtp_stream_add_pcm_hw_constraints(stream, runtime);
end:
	return err;
}

static int pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_oxfw *oxfw = substream->private_data;
	unsigned int rate;
	int err;

	err = snd_oxfw_stream_lock_try(oxfw);
	if (err < 0)
		goto end;

	err = init_hw_params(oxfw, substream);
	if (err < 0)
		goto err_locked;

	/*
	 * When any PCM streams are already running, the available sampling
	 * rate is limited at current value.
	 */
	if (amdtp_stream_pcm_running(&oxfw->tx_stream) ||
	    amdtp_stream_pcm_running(&oxfw->rx_stream)) {
		err = snd_oxfw_stream_get_rate(oxfw, &rate);
		if (err < 0) {
			dev_err(&oxfw->unit->device,
				"fail to get sampling rate: %d\n", err);
			goto err_locked;
		}
		substream->runtime->hw.rate_min = rate;
		substream->runtime->hw.rate_max = rate;
	}

	snd_pcm_set_sync(substream);
end:
	return err;
err_locked:
	snd_oxfw_stream_lock_release(oxfw);
	return err;
}

static int pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_oxfw *oxfw = substream->private_data;
	snd_oxfw_stream_lock_release(oxfw);
	return 0;
}

static int pcm_capture_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct snd_oxfw *oxfw = substream->private_data;

	if (substream->runtime->status->state == SNDRV_PCM_STATE_OPEN)
		atomic_inc(&oxfw->capture_substreams);
	amdtp_stream_set_pcm_format(&oxfw->tx_stream, params_format(hw_params));

	return snd_pcm_lib_alloc_vmalloc_buffer(substream,
						params_buffer_bytes(hw_params));
}
static int pcm_playback_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *hw_params)
{
	struct snd_oxfw *oxfw = substream->private_data;

	if (substream->runtime->status->state == SNDRV_PCM_STATE_OPEN)
		atomic_inc(&oxfw->playback_substreams);
	amdtp_stream_set_pcm_format(&oxfw->rx_stream, params_format(hw_params));

	return snd_pcm_lib_alloc_vmalloc_buffer(substream,
						params_buffer_bytes(hw_params));
}

static int pcm_capture_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_oxfw *oxfw = substream->private_data;

	if (substream->runtime->status->state != SNDRV_PCM_STATE_OPEN)
		atomic_dec(&oxfw->capture_substreams);

	snd_oxfw_stream_stop_simplex(oxfw, &oxfw->tx_stream);

	return snd_pcm_lib_free_vmalloc_buffer(substream);
}
static int pcm_playback_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_oxfw *oxfw = substream->private_data;

	if (substream->runtime->status->state != SNDRV_PCM_STATE_OPEN)
		atomic_dec(&oxfw->playback_substreams);

	snd_oxfw_stream_stop_simplex(oxfw, &oxfw->rx_stream);

	return snd_pcm_lib_free_vmalloc_buffer(substream);
}

static int pcm_capture_prepare(struct snd_pcm_substream *substream)
{
	struct snd_oxfw *oxfw = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;

	err = snd_oxfw_stream_start_simplex(oxfw, &oxfw->tx_stream,
					    runtime->rate, runtime->channels);
	if (err < 0)
		goto end;

	amdtp_stream_pcm_prepare(&oxfw->tx_stream);
end:
	return err;
}
static int pcm_playback_prepare(struct snd_pcm_substream *substream)
{
	struct snd_oxfw *oxfw = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;

	err = snd_oxfw_stream_start_simplex(oxfw, &oxfw->rx_stream,
					    runtime->rate, runtime->channels);
	if (err < 0)
		goto end;

	amdtp_stream_pcm_prepare(&oxfw->rx_stream);
end:
	return err;
}

static int pcm_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_oxfw *oxfw = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		amdtp_stream_pcm_trigger(&oxfw->tx_stream, substream);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		amdtp_stream_pcm_trigger(&oxfw->tx_stream, NULL);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
static int pcm_playback_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_oxfw *oxfw = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		amdtp_stream_pcm_trigger(&oxfw->rx_stream, substream);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		amdtp_stream_pcm_trigger(&oxfw->rx_stream, NULL);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t pcm_capture_pointer(struct snd_pcm_substream *sbstm)
{
	struct snd_oxfw *oxfw = sbstm->private_data;

	return amdtp_stream_pcm_pointer(&oxfw->tx_stream);
}
static snd_pcm_uframes_t pcm_playback_pointer(struct snd_pcm_substream *sbstm)
{
	struct snd_oxfw *oxfw = sbstm->private_data;

	return amdtp_stream_pcm_pointer(&oxfw->rx_stream);
}

int snd_oxfw_create_pcm(struct snd_oxfw *oxfw)
{
	static struct snd_pcm_ops capture_ops = {
		.open      = pcm_open,
		.close     = pcm_close,
		.ioctl     = snd_pcm_lib_ioctl,
		.hw_params = pcm_capture_hw_params,
		.hw_free   = pcm_capture_hw_free,
		.prepare   = pcm_capture_prepare,
		.trigger   = pcm_capture_trigger,
		.pointer   = pcm_capture_pointer,
		.page      = snd_pcm_lib_get_vmalloc_page,
		.mmap      = snd_pcm_lib_mmap_vmalloc,
	};
	static struct snd_pcm_ops playback_ops = {
		.open      = pcm_open,
		.close     = pcm_close,
		.ioctl     = snd_pcm_lib_ioctl,
		.hw_params = pcm_playback_hw_params,
		.hw_free   = pcm_playback_hw_free,
		.prepare   = pcm_playback_prepare,
		.trigger   = pcm_playback_trigger,
		.pointer   = pcm_playback_pointer,
		.page      = snd_pcm_lib_get_vmalloc_page,
		.mmap      = snd_pcm_lib_mmap_vmalloc,
	};
	struct snd_pcm *pcm;
	unsigned int cap = 0;
	int err;

	if (oxfw->has_output)
		cap = 1;

	err = snd_pcm_new(oxfw->card, oxfw->card->driver, 0, 1, cap, &pcm);
	if (err < 0)
		return err;

	pcm->private_data = oxfw;
	strcpy(pcm->name, oxfw->card->shortname);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &playback_ops);
	if (cap > 0)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &capture_ops);

	return 0;
}