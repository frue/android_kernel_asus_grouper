#include "somagic.h"

static const int check_audio_sync = SOMAGIC_AUDIO_CHECK_SYNC;

static const struct snd_pcm_hardware pcm_hardware = {
  .info = SNDRV_PCM_INFO_BLOCK_TRANSFER |
					SNDRV_PCM_INFO_MMAP |
					SNDRV_PCM_INFO_INTERLEAVED |
					SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S32_LE, //SNDRV_PCM_FMTBIT_S24_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 65280, // 1020Bytes * 32Packets * 2 Periods
	.period_bytes_min = 32640, // 1020Bytes * 32Packets * 1 Periods
	.period_bytes_max = 65280, // 1020Bytes * 32Packets * 2 Periods
	.periods_min = 1,
	.periods_max = 2
};

static int somagic_pcm_open(struct snd_pcm_substream *substream)
{
	struct usb_somagic *somagic = snd_pcm_substream_chip(substream);
	if (!somagic) {
		return -ENODEV;
	}

	printk(KERN_INFO "somagic::%s Called!, %d users\n", __func__,
				 somagic->audio.users);

	somagic->audio.users++;
	somagic->audio.pcm_substream = substream;
	substream->runtime->private_data = somagic;
	substream->runtime->hw = pcm_hardware;

	snd_pcm_hw_constraint_integer(substream->runtime,
																SNDRV_PCM_HW_PARAM_PERIODS);

	somagic->audio.dma_offset = 0;
	somagic->audio.packets = 0;
	somagic_start_stream(somagic);

	return 0;
}

static int somagic_pcm_close(struct snd_pcm_substream *substream)
{
	struct usb_somagic *somagic = snd_pcm_substream_chip(substream);
	unsigned long lock_flags;

	printk(KERN_INFO "somagic::%s Called!, %d users\n", __func__,
				 somagic->audio.users);

	/* Clear flag, in case it's not allready done! */
	spin_lock_irqsave(&somagic->streaming_flags_lock, lock_flags);
	somagic->streaming_flags &= ~SOMAGIC_STREAMING_CAPTURE_AUDIO;
	spin_unlock_irqrestore(&somagic->streaming_flags_lock, lock_flags);

	somagic_stop_stream(somagic);

	somagic->audio.users--;
	return 0;
}

static int somagic_pcm_hw_params(struct snd_pcm_substream *substream,
																 struct snd_pcm_hw_params *hw_params)
{
	printk(KERN_INFO "somagic::%s: Allocating %d bytes buffer\n",
				 __func__, params_buffer_bytes(hw_params));
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
}

static int somagic_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct usb_somagic *somagic = snd_pcm_substream_chip(substream);
	unsigned long lock_flags;

	/* Clear flag, in case it's not allready done! */
	spin_lock_irqsave(&somagic->streaming_flags_lock, lock_flags);
	somagic->streaming_flags &= ~SOMAGIC_STREAMING_CAPTURE_AUDIO;
	spin_unlock_irqrestore(&somagic->streaming_flags_lock, lock_flags);
	somagic_stop_stream(somagic);
	return snd_pcm_lib_free_pages(substream);
}

static int somagic_pcm_prepare(struct snd_pcm_substream *substream)
{
	return 0;
}

/*
 * somagic_pcm_trigger
 *
 * Alsa Callback
 *
 * WARNING: This callback is ATOMIC, and must not sleep
 */
static int somagic_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	unsigned long lock_flags;
	struct usb_somagic *somagic = snd_pcm_substream_chip(substream);
	switch(cmd) {
		case SNDRV_PCM_TRIGGER_START: {
			spin_lock_irqsave(&somagic->streaming_flags_lock, lock_flags);
			somagic->streaming_flags |= SOMAGIC_STREAMING_CAPTURE_AUDIO;
			spin_unlock_irqrestore(&somagic->streaming_flags_lock, lock_flags);
			somagic->audio.dma_offset = 0;
			somagic->audio.packets = 0;
			return 0;
		}
		case SNDRV_PCM_TRIGGER_STOP: {
			spin_lock_irqsave(&somagic->streaming_flags_lock, lock_flags);
			somagic->streaming_flags &= ~SOMAGIC_STREAMING_CAPTURE_AUDIO;
			spin_unlock_irqrestore(&somagic->streaming_flags_lock, lock_flags);
			return 0;
		}
		default: {
			return -EINVAL;
		}
	}
	return -EINVAL;
}

/* 
 * somagic_pcm_pointer
 *
 * Alsa Callback
 *
 * WARNING: This callback is atomic, and must not sleep
 */
static snd_pcm_uframes_t somagic_pcm_pointer(
																					struct snd_pcm_substream *substream)
{
	struct usb_somagic *somagic = snd_pcm_substream_chip(substream);

	return somagic->audio.dma_write_ptr / 8;
}

static struct snd_pcm_ops somagic_audio_ops = {
	.open = somagic_pcm_open,
	.close = somagic_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = somagic_pcm_hw_params,
	.hw_free = somagic_pcm_hw_free,
	.prepare = somagic_pcm_prepare,
	.trigger = somagic_pcm_trigger,
	.pointer = somagic_pcm_pointer,
};

/*
 * process_audio
 *
 * Tasklet, called after isochrounous usb-transfer
 * WARNING: This is bottom-half of interrupt, and must not sleep!
 */
static void process_audio(unsigned long somagic_addr)
{
	struct usb_somagic *somagic = (struct usb_somagic *)somagic_addr;

	if (!somagic->audio.elapsed_periode) {
		return;
	}

	snd_pcm_period_elapsed(somagic->audio.pcm_substream);
	somagic->audio.elapsed_periode = 0;
}

int somagic_alsa_init(struct usb_somagic *somagic)
{
	int rc;
	struct snd_card *sound_card;
	struct snd_pcm *sound_pcm;

	tasklet_init(&(somagic->audio.process_audio), process_audio, (unsigned long)somagic);

	rc = snd_card_create(SNDRV_DEFAULT_IDX1, "Somagic",
											THIS_MODULE, 0, &sound_card);
	if (rc != 0) {
		printk(KERN_ERR "somagic::%s: Could not do ALSA snd_card_create()\n",
					 __func__);
		return rc;
	}

	rc = snd_pcm_new(sound_card, "Somagic PCM", 0, 0, 1, &sound_pcm);
	if (rc != 0) {
		printk(KERN_ERR "somagic::%s: Could not do ALSA snd_pcm_new()\n",
					 __func__);
		return rc;
	}
	snd_pcm_set_ops(sound_pcm, SNDRV_PCM_STREAM_CAPTURE, &somagic_audio_ops);
	sound_pcm->info_flags = 0;
	sound_pcm->private_data = somagic;
	strcpy(sound_pcm->name, "Somagic PCM");

	snd_pcm_lib_preallocate_pages_for_all(sound_pcm, SNDRV_DMA_TYPE_CONTINUOUS,
																				snd_dma_continuous_data(GFP_KERNEL),
																				65280, 65280);
	
	strcpy(sound_card->driver, "ALSA driver");
	strcpy(sound_card->shortname, "somagic audio");
	strcpy(sound_card->longname, "somagic ALSA audio");

	rc = snd_card_register(sound_card);
	if (rc != 0) {
		printk(KERN_ERR "somagic::%s: Could not do ALSA snd_card_register()\n",
					 __func__);
		
		snd_card_free(sound_card);
		return rc;
	}


	printk(KERN_INFO "somagic::%s: Successfully registered audio device!\n",
				 __func__);

	somagic->audio.card = sound_card;
	return 0;
}

void __devexit somagic_alsa_exit(struct usb_somagic *somagic)
{
	tasklet_kill(&(somagic->audio.process_audio));

	if (somagic->audio.card != NULL) {
		snd_card_free(somagic->audio.card);
	}
}

/*
 * somagic_audio_put
 *
 * Called when the driver is recieving data in the isochronous usb-transfer
 *
 * WARNING: This function is called within an interrupt, don't let it sleep
 *
 * The audio data is received as Signed 32Bit from the device with LSB = 0x00
 * We use this fact to detect the beginning of a sample.
 *
 */
void somagic_audio_put(struct usb_somagic *somagic, u8 *data, int len)
{
	struct snd_pcm_runtime *runtime;
	int len_part, i;
	int offset = 0;

	if (!(somagic->streaming_flags & SOMAGIC_STREAMING_CAPTURE_AUDIO)) {
		return;
	}

	runtime = somagic->audio.pcm_substream->runtime;

	if (len != 1020 && printk_ratelimit()) {
		printk(KERN_INFO "somagic::%s (%d): Got strange length of audio data: %d\n",
					 __func__, __LINE__, len);
	}

	somagic->audio.packets++;

	/* 
 	 * We check that we are feeding alsa clean audio on every Nth packet.
 	 * TODO: We should make the check with increased intervals if the audio is clean
 	 */
	if (somagic->audio.packets % check_audio_sync && somagic->audio.dma_write_ptr > 4) {
		if (data[0 + somagic->audio.dma_offset] != 0x00 && printk_ratelimit()) {
			printk(KERN_INFO "somagic::%s (%d): Lost sync??\n",
					 	 __func__, __LINE__);

			i = 0;
			while(len > 0 && *data != 0x00) {
				i++;
				data++;
				len--;
			}

			if (len == 0) {
				if (printk_ratelimit()) {
					printk(KERN_INFO "somagic::%s (%d): Could not find sync!\n",
							 	 __func__, __LINE__);
				}
				return;
			}

			offset = i % 4;
			somagic->audio.dma_offset = offset;
			// We reset the write pointer when we change the offset
			somagic->audio.dma_write_ptr = 0;
		
			if (printk_ratelimit()) {
					printk(KERN_INFO "somagic::%s (%d): Found sync at offset %d\n",
							 	 __func__, __LINE__, somagic->audio.dma_offset);
			}
		}
	}


	if (somagic->audio.dma_write_ptr + len < runtime->dma_bytes) {
		memcpy(runtime->dma_area + somagic->audio.dma_write_ptr, data, len);
		somagic->audio.dma_write_ptr += len;
	} else {
		len_part = runtime->dma_bytes - somagic->audio.dma_write_ptr;
		memcpy(runtime->dma_area + somagic->audio.dma_write_ptr, data, len_part);

		if (len == len_part) {
			somagic->audio.dma_write_ptr = 0;
		} else {
			memcpy(runtime->dma_area, data + len_part, len - len_part);
			somagic->audio.dma_write_ptr = len - len_part;
		}
	}

	somagic->audio.elapsed_periode = 1;
}
 
