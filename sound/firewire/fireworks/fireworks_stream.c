/*
 * fireworks_stream.c - a part of driver for Fireworks based devices
 *
 * Copyright (c) 2013 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */
#include "./fireworks.h"

static int
init_stream(struct snd_efw *efw, struct amdtp_stream *stream)
{
	struct cmp_connection *conn;
	enum cmp_direction c_dir;
	enum amdtp_stream_direction s_dir;
	int err;

	if (stream == &efw->tx_stream) {
		conn = &efw->out_conn;
		c_dir = CMP_OUTPUT;
		s_dir = AMDTP_IN_STREAM;
	} else {
		conn = &efw->in_conn;
		c_dir = CMP_INPUT;
		s_dir = AMDTP_OUT_STREAM;
	}

	err = cmp_connection_init(conn, efw->unit, c_dir, 0);
	if (err < 0)
		goto end;

	err = amdtp_stream_init(stream, efw->unit, s_dir, CIP_BLOCKING);
	if (err < 0) {
		cmp_connection_destroy(conn);
		goto end;
	}

end:
	return err;
}

static void
stop_stream(struct snd_efw *efw, struct amdtp_stream *stream)
{
	if (amdtp_stream_running(stream))
		amdtp_stream_stop(stream);

	if (stream == &efw->tx_stream)
		cmp_connection_break(&efw->out_conn);
	else
		cmp_connection_break(&efw->in_conn);

	return;
}

static int
start_stream(struct snd_efw *efw, struct amdtp_stream *stream,
	     unsigned int sampling_rate)
{
	struct cmp_connection *conn;
	unsigned int pcm_channels, midi_ports;
	int mode, err;

	/* already running */
	if (amdtp_stream_running(stream)) {
		err = 0;
		goto end;
	}

	mode = snd_efw_get_multiplier_mode(sampling_rate);
	if (stream == &efw->tx_stream) {
		conn = &efw->out_conn;
		pcm_channels = efw->pcm_capture_channels[mode];
		midi_ports = efw->midi_out_ports;
	} else {
		conn = &efw->in_conn;
		pcm_channels = efw->pcm_playback_channels[mode];
		midi_ports = efw->midi_in_ports;
	}

	amdtp_stream_set_parameters(stream, sampling_rate,
				    pcm_channels, midi_ports);

	/*  establish connection via CMP */
	err = cmp_connection_establish(conn,
				amdtp_stream_get_max_payload(stream));
	if (err < 0)
		goto end;

	/* start amdtp stream */
	err = amdtp_stream_start(stream,
				 conn->resources.channel,
				 conn->speed);
	if (err < 0)
		stop_stream(efw, stream);

	/* wait first callback */
	if (!amdtp_stream_wait_callback(stream)) {
		stop_stream(efw, stream);
		err = -ETIMEDOUT;
		goto end;
	}
end:
	return err;
}

static void
update_stream(struct snd_efw *efw, struct amdtp_stream *stream)
{
	struct cmp_connection *conn;

	if (&efw->tx_stream == stream)
		conn = &efw->out_conn;
	else
		conn = &efw->in_conn;

	if (cmp_connection_update(conn) < 0) {
		amdtp_stream_pcm_abort(stream);
		mutex_lock(&efw->mutex);
		stop_stream(efw, stream);
		mutex_unlock(&efw->mutex);
		return;
	}
	amdtp_stream_update(stream);
}

static void
destroy_stream(struct snd_efw *efw, struct amdtp_stream *stream)
{
	stop_stream(efw, stream);

	if (stream == &efw->tx_stream)
		cmp_connection_destroy(&efw->out_conn);
	else
		cmp_connection_destroy(&efw->in_conn);
}

static int
get_roles(struct snd_efw *efw, enum cip_flags *sync_mode,
	  struct amdtp_stream **master, struct amdtp_stream **slave)
{
	enum snd_efw_clock_source clock_source;
	int err;

	err = snd_efw_command_get_clock_source(efw, &clock_source);
	if (err < 0)
		goto end;

	if (clock_source != SND_EFW_CLOCK_SOURCE_SYTMATCH) {
		*master = &efw->tx_stream;
		*slave = &efw->rx_stream;
		*sync_mode = CIP_SYNC_TO_DEVICE;
	} else {
		err = -ENOSYS;
	}
end:
	return err;
}

static int
check_connection_used_by_others(struct snd_efw *efw,
				struct amdtp_stream *s, bool *used)
{
	struct cmp_connection *conn;
	int err;

	if (s == &efw->tx_stream)
		conn = &efw->out_conn;
	else
		conn = &efw->in_conn;

	err = cmp_connection_check_used(conn, used);
	if (err >= 0)
		*used = (*used && !amdtp_stream_running(s));

	return err;
}

int snd_efw_stream_init_duplex(struct snd_efw *efw)
{
	int err;

	err = init_stream(efw, &efw->tx_stream);
	if (err < 0)
		goto end;

	err = init_stream(efw, &efw->rx_stream);
	if (err < 0)
		goto end;

	/* set IEC61883 compliant mode */
	err = snd_efw_command_set_tx_mode(efw, SND_EFW_TRANSPORT_MODE_IEC61883);
end:
	return err;
}

int snd_efw_stream_start_duplex(struct snd_efw *efw,
				struct amdtp_stream *request,
				int sampling_rate)
{
	struct amdtp_stream *master, *slave;
	enum cip_flags sync_mode;
	int err, curr_rate;
	bool slave_flag, used;

	err = get_roles(efw, &sync_mode, &master, &slave);
	if (err < 0)
		goto end;

	if ((request == slave) || amdtp_stream_running(slave))
		slave_flag = true;
	else
		slave_flag = false;

	/*
	 * Considering JACK/FFADO streaming:
	 * TODO: This can be removed hwdep functionality becomes popular.
	 */
	err = check_connection_used_by_others(efw, master, &used);
	if (err < 0)
		goto end;
	if (used) {
		dev_err(&efw->unit->device,
			"connections established by others: %d\n",
			used);
		err = -EBUSY;
		goto end;
	}

	/* change sampling rate if possible */
	err = snd_efw_command_get_sampling_rate(efw, &curr_rate);
	if (err < 0)
		goto end;
	if (sampling_rate == 0)
		sampling_rate = curr_rate;
	if (sampling_rate != curr_rate) {
		/* master is just for MIDI stream */
		if (amdtp_stream_running(master) &&
		    !amdtp_stream_pcm_running(master))
			stop_stream(efw, master);

		/* slave is just for MIDI stream */
		if (amdtp_stream_running(slave) &&
		    !amdtp_stream_pcm_running(slave))
			stop_stream(efw, slave);

		err = snd_efw_command_set_sampling_rate(efw, sampling_rate);
		if (err < 0)
			goto end;
	}

	/*  master should be always running */
	if (!amdtp_stream_running(master)) {
		amdtp_stream_set_sync(sync_mode, master, slave);
		err = start_stream(efw, master, sampling_rate);
		if (err < 0) {
			dev_err(&efw->unit->device,
				"fail to start AMDTP master stream:%d\n", err);
			goto end;
		}
	}

	/* start slave if needed */
	if (slave_flag && !amdtp_stream_running(slave)) {
		err = start_stream(efw, slave, sampling_rate);
		if (err < 0) {
			dev_err(&efw->unit->device,
				"fail to start AMDTP slave stream:%d\n", err);
			goto end;
		}
	}
end:
	return err;
}

int snd_efw_stream_stop_duplex(struct snd_efw *efw)
{
	struct amdtp_stream *master, *slave;
	enum cip_flags sync_mode;
	int err;

	err = get_roles(efw, &sync_mode, &master, &slave);
	if (err < 0)
		goto end;

	if (amdtp_stream_pcm_running(slave) ||
	    amdtp_stream_midi_running(slave))
		goto end;

	stop_stream(efw, slave);

	if (!amdtp_stream_pcm_running(master) &&
	    !amdtp_stream_midi_running(master))
		stop_stream(efw, master);

end:
	return err;
}

void snd_efw_stream_update_duplex(struct snd_efw *efw)
{
	update_stream(efw, &efw->rx_stream);
	update_stream(efw, &efw->tx_stream);
}

void snd_efw_stream_destroy_duplex(struct snd_efw *efw)
{
	if (amdtp_stream_pcm_running(&efw->rx_stream))
		amdtp_stream_pcm_abort(&efw->rx_stream);
	if (amdtp_stream_pcm_running(&efw->tx_stream))
		amdtp_stream_pcm_abort(&efw->tx_stream);

	destroy_stream(efw, &efw->rx_stream);
	destroy_stream(efw, &efw->tx_stream);
}

void snd_efw_stream_lock_changed(struct snd_efw *efw)
{
	efw->dev_lock_changed = true;
	wake_up(&efw->hwdep_wait);
}

int snd_efw_stream_lock_try(struct snd_efw *efw)
{
	int err;

	spin_lock_irq(&efw->lock);

	/* user land lock this */
	if (efw->dev_lock_count < 0) {
		err = -EBUSY;
		goto end;
	}

	/* this is the first time */
	if (efw->dev_lock_count++ == 0)
		snd_efw_stream_lock_changed(efw);
	err = 0;
end:
	spin_unlock_irq(&efw->lock);
	return err;
}

void snd_efw_stream_lock_release(struct snd_efw *efw)
{
	spin_lock_irq(&efw->lock);

	if (WARN_ON(efw->dev_lock_count <= 0))
		goto end;
	if (--efw->dev_lock_count == 0)
		snd_efw_stream_lock_changed(efw);
end:
	spin_unlock_irq(&efw->lock);
}
