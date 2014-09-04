/*
 * Copyright 2013-2014 Con Kolivas
 * Copyright 2014 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <string.h>

#include "miner.h"
#include "deviceapi.h"
#include "driver-cointerra.h"
#include "lowlevel.h"
#include "lowl-usb.h"
#include <math.h>

static const unsigned cointerra_desired_roll = 60;
static const unsigned long cointerra_latest_result_usecs = (10 * 1000000);

BFG_REGISTER_DRIVER(cointerra_drv)

static const char *cointerra_hdr = "ZZ";

int opt_ps_load;

static void cta_gen_message(char *msg, char type)
{
	memset(msg, 0, CTA_MSG_SIZE);
	memcpy(msg, cointerra_hdr, 2);
	msg[CTA_MSG_TYPE] = type;
}

/* Find the number of leading zero bits in diff */
static uint8_t diff_to_bits(double diff)
{
	uint64_t diff64;
	uint8_t i;

	diff *= (double)2147483648.0;
	if (diff > 0x8000000000000000ULL)
		diff = 0x8000000000000000ULL;
	/* Convert it to an integer */
	diff64 = diff;
	for (i = 0; diff64; i++, diff64 >>= 1);

	return i;
}

static double bits_to_diff(uint8_t bits)
{
	double ret = 1.0;

	if (likely(bits > 32))
		ret *= 1ull << (bits - 32);
	else if (unlikely(bits < 32))
		ret /= 1ull << (32 - bits);
	return ret;
}

static bool cta_reset_init(char *buf)
{
	return ((buf[CTA_MSG_TYPE] == CTA_RECV_RDONE) && ((buf[CTA_RESET_TYPE]&0x3) == CTA_RESET_INIT));
}

static char *mystrstr(char *haystack, int size, const char *needle)
{
	int loop = 0;

	while (loop < (size-1)) {
		if ((haystack[loop] == needle[0])&&
		    (haystack[loop+1] == needle[1]))
			return &haystack[loop];
		loop++;
	}
	return NULL;
}

static
bool cta_open(struct lowl_usb_endpoint * const ep, const char * const repr)
{
	int amount, offset = 0;
	char buf[CTA_MSG_SIZE];
	cgtimer_t ts_start;
	bool ret = false;

	applog(LOG_INFO, "CTA_OPEN");

	cta_gen_message(buf, CTA_SEND_RESET);
	// set the initial difficulty
	buf[CTA_RESET_TYPE] = CTA_RESET_INIT | CTA_RESET_DIFF;
	buf[CTA_RESET_DIFF] = diff_to_bits(CTA_INIT_DIFF);
	buf[CTA_RESET_LOAD] = opt_cta_load ? opt_cta_load : 255;
	buf[CTA_RESET_PSLOAD] = opt_ps_load;

	amount = usb_write(ep, buf, CTA_MSG_SIZE);
	if (amount != CTA_MSG_SIZE) {
		applog(LOG_INFO, "Write error %s, wrote %d of %d",
		       bfg_strerror(errno, BST_ERRNO),
		       amount, CTA_MSG_SIZE);
		return ret;
	}

	cgtimer_time(&ts_start);

	/* Read from the device for up to 2 seconds discarding any data that
	 * doesn't match a reset complete acknowledgement. */
	while (42) {
		cgtimer_t ts_now, ts_diff;
		char *msg;

		cgtimer_time(&ts_now);
		cgtimer_sub(&ts_now, &ts_start, &ts_diff);
		if (cgtimer_to_ms(&ts_diff) > 2000) {
			applog(LOG_DEBUG, "%s: Timed out waiting for response to reset init", repr);
			break;
		}

		amount = usb_read(ep, buf + offset, CTA_MSG_SIZE - offset);
		if (amount != (CTA_MSG_SIZE - offset) && amount != 0) {
			applog(LOG_INFO, "%s: Read error %s, read %d",
			       repr, bfg_strerror(errno, BST_ERRNO), amount);
			break;
		}
		if (!amount)
			continue;

		msg = mystrstr(buf, amount, cointerra_hdr);
		if (!msg) {
			/* Keep the last byte in case it's the first byte of
			 * the 2 byte header. */
			offset = 1;
			memmove(buf, buf + amount - 1, offset);
			continue;
		}

		if (msg > buf) {
			/* length of message = offset for next usb_read after moving */
			offset = CTA_MSG_SIZE - (msg - buf);
			memmove(buf, msg, offset);
			continue;
		}

		/* We have a full sized message starting with the header now */
		if (cta_reset_init(buf)) {
			/* We can't store any other data returned with this
			 * reset since we have not allocated any memory for
			 * a cointerra_info structure yet. */
			applog(LOG_INFO, "%s: Successful reset init received", repr);
			ret = true;
			break;
		}
	}

	return ret;
}

static
bool cointerra_open(const struct lowlevel_device_info * const info, const char * const repr, struct libusb_device_handle ** const usbh_p, struct lowl_usb_endpoint ** const ep_p)
{
	if (libusb_open(info->lowl_data, usbh_p))
		applogr(false, LOG_DEBUG, "%s: USB open failed on %s",
		        cointerra_drv.dname, info->devid);
	*ep_p = usb_open_ep_pair(*usbh_p, LIBUSB_ENDPOINT_IN | 1, 64, LIBUSB_ENDPOINT_OUT | 1, 64);
	if (!*ep_p)
	{
		applog(LOG_DEBUG, "%s: Endpoint open failed on %s",
		       cointerra_drv.dname, info->devid);
fail:
		libusb_close(*usbh_p);
		*usbh_p = NULL;
		return false;
	}
	
	if (!cta_open(*ep_p, repr))
	{
		usb_close_ep(*ep_p);
		*ep_p = NULL;
		goto fail;
	}
	
	return true;
}

static void cta_clear_work(struct cgpu_info *cgpu)
{
	struct work *work, *tmp;

	wr_lock(&cgpu->qlock);
	HASH_ITER(hh, cgpu->queued_work, work, tmp) {
		__work_completed(cgpu, work);
		free_work(work);
	}
	wr_unlock(&cgpu->qlock);
}

static void cta_close(struct cgpu_info *cointerra)
{
	struct cointerra_info *info = cointerra->device_data;

	/* Wait for read thread to die */
	pthread_join(info->read_thr, NULL);

	/* Open does the same reset init followed by response as is required to
	 * close the device. */
	if (!cta_open(info->ep, cointerra->dev_repr)) {
		applog(LOG_INFO, "%s %d: Reset on close failed", cointerra->drv->name,
			cointerra->device_id);
	}

	mutex_destroy(&info->lock);
	mutex_destroy(&info->sendlock);
	/* Don't free info here to avoid trying to access dereferenced members
	 * once a device is unplugged. */
	cta_clear_work(cointerra);
}

static void cta_parse_info(struct cgpu_info *, struct cointerra_info *, char *);
static void msg_from_hu16(char *, int, uint16_t);

static
bool cointerra_wait_for_info(struct cointerra_info * const ctainfo, struct lowl_usb_endpoint * const ep)
{
	char buf[CTA_MSG_SIZE];
	int amount;
	
	cta_gen_message(buf, CTA_SEND_REQUEST);
	msg_from_hu16(buf, CTA_REQ_MSGTYPE, CTA_RECV_INFO);
	msg_from_hu16(buf, CTA_REQ_INTERVAL, 0);
	amount = usb_write(ep, buf, CTA_MSG_SIZE);
	if (amount != CTA_MSG_SIZE)
		return false;
	
	do {
		amount = usb_read(ep, buf, CTA_MSG_SIZE);
		if (amount != CTA_MSG_SIZE)
			applogr(false, LOG_ERR, "%s: Read error %s, read %d",
			        __func__, bfg_strerror(errno, BST_ERRNO), amount);
		
		if (memcmp(buf, cointerra_hdr, 2))
			applogr(false, LOG_ERR, "%s: Packet header mismatch", __func__);
	} while (buf[CTA_MSG_TYPE] != CTA_RECV_INFO);
	
	cta_parse_info(NULL, ctainfo, buf);
	
	return true;
}

static
bool cointerra_lowl_probe(const struct lowlevel_device_info * const info)
{
	struct cointerra_info ctainfo;
	struct libusb_device_handle *usbh;
	struct lowl_usb_endpoint *ep;
	bool b;
	
	if (!cointerra_open(info, cointerra_drv.dname, &usbh, &ep))
		return false;
	mutex_init(&ctainfo.lock);
	b = cointerra_wait_for_info(&ctainfo, ep);
	mutex_destroy(&ctainfo.lock);
	usb_close_ep(ep);
	libusb_close(usbh);
	
	if (!b)
		return false;
	
	applog(LOG_DEBUG, "%s: Found %lu cores on %s",
	       __func__, (unsigned long)ctainfo.cores, info->devid);

	struct cgpu_info * const dev = malloc(sizeof(*dev));
	*dev = (struct cgpu_info){
		.drv = &cointerra_drv,
		.procs = ctainfo.cores,
		.device_data = lowlevel_ref(info),
		.threads = 1,
		.device_path = strdup(info->devid),
		.dev_manufacturer = maybe_strdup(info->manufacturer),
		.dev_product = maybe_strdup(info->product),
		.dev_serial = maybe_strdup(info->serial),
		.deven = DEV_ENABLED,
		.min_nonce_diff = CTA_INIT_DIFF,
	};
	const bool rv = add_cgpu(dev);
	applog(LOG_INFO, "%s: Successfully set up %s",
	       cointerra_drv.dname, dev->dev_repr);
	return rv;
}

static
bool cointerra_lowl_match(const struct lowlevel_device_info * const info)
{
	return lowlevel_match_lowlproduct(info, &lowl_usb, "GoldStrike");
}

/* This function will remove a work item from the hashtable if it matches the
 * id in work->subid and return a pointer to the work but it will not free the
 * work. It may return NULL if it cannot find matching work. */
static struct work *take_work_by_id(struct cgpu_info *cgpu, uint16_t id)
{
	struct work *work, *tmp, *ret = NULL;

	wr_lock(&cgpu->qlock);
	HASH_ITER(hh, cgpu->queued_work, work, tmp) {
		if (work->subid == id) {
			ret = work;
			break;
		}
	}
	if (ret)
		__work_completed(cgpu, ret);
	wr_unlock(&cgpu->qlock);

	return ret;
}

/* This function will look up a work item in the hashtable if it matches the
 * id in work->subid and return a cloned work item if it matches. It may return
 * NULL if it cannot find matching work. */
static struct work *clone_work_by_id(struct cgpu_info *cgpu, uint16_t id)
{
	struct work *work, *tmp, *ret = NULL;

	rd_lock(&cgpu->qlock);
	HASH_ITER(hh, cgpu->queued_work, work, tmp) {
		if (work->subid == id) {
			ret = work;
			break;
		}
	}
	if (ret)
		ret = copy_work(ret);
	rd_unlock(&cgpu->qlock);

	return ret;
}

static bool cta_send_msg(struct cgpu_info *cointerra, char *buf);

static uint16_t hu16_from_msg(char *buf, int msg)
{
	return le16toh(*(uint16_t *)&buf[msg]);
}

static uint32_t hu32_from_msg(char *buf, int msg)
{
	return le32toh(*(uint32_t *)&buf[msg]);
}

static uint64_t hu64_from_msg(char *buf, int msg)
{
	return le64toh(*(uint64_t *)&buf[msg]);
}

static uint8_t u8_from_msg(char *buf, int msg)
{
	return *(uint8_t *)&buf[msg];
}

static void msg_from_hu16(char *buf, int msg, uint16_t val)
{
	*(uint16_t *)&buf[msg] = htole16(val);
}

static void cta_parse_reqwork(struct cgpu_info *cointerra, struct cointerra_info *info,
			      char *buf)
{
	uint16_t retwork;

	retwork = hu16_from_msg(buf, CTA_REQWORK_REQUESTS);
	applog(LOG_DEBUG, "%s %d: Request work message for %u items received",
	       cointerra->drv->name, cointerra->device_id, retwork);

	mutex_lock(&info->lock);
	info->requested = retwork;
	/* Wake up the main scanwork loop since we need more
		* work. */
	pthread_cond_signal(&info->wake_cond);
	mutex_unlock(&info->lock);
}

static void cta_parse_recvmatch(struct thr_info *thr, struct cgpu_info *cointerra,
				struct cointerra_info *info, char *buf)
{
	struct cgpu_info *corecgpu;
	struct thr_info *corethr;
	uint32_t timestamp_offset, mcu_tag;
	uint16_t retwork;
	struct work *work;
	uint8_t asic, core, pipe, coreno;
	int pipeno, bitchar, bitbit;

	/* No endian switch needs doing here since it's sent and returned as
	 * the same 4 bytes */
	retwork = *(uint16_t *)(&buf[CTA_DRIVER_TAG]);
	mcu_tag = hu32_from_msg(buf, CTA_MCU_TAG);
	const uint8_t wdiffbits = u8_from_msg(buf, CTA_WORK_DIFFBITS);
	const uint32_t nonce = hu32_from_msg(buf, CTA_MATCH_NONCE);
	
	asic = u8_from_msg(buf, CTA_MCU_ASIC);
	core = u8_from_msg(buf, CTA_MCU_CORE);
	pipe = u8_from_msg(buf, CTA_MCU_PIPE);
	pipeno = asic * 512 + core * 128 + pipe;
	
	corecgpu = cointerra;
	for (int i = 0; i < pipeno; ++i)
	{
		corecgpu = corecgpu->next_proc;
		if (unlikely(!corecgpu))
		{
			corecgpu = cointerra;
			break;
		}
	}
	corethr = corecgpu->thr[0];
	
	applog(LOG_DEBUG, "%s %d: Match message for id 0x%04x MCU id 0x%08x received",
	       cointerra->drv->name, cointerra->device_id, retwork, mcu_tag);

	work = clone_work_by_id(cointerra, retwork);
	if (likely(work)) {
		unsigned char rhash[32];
		char outhash[16];
		double wdiff;
		uint64_t hashes;
		bool ret;

		timestamp_offset = hu32_from_msg(buf, CTA_MATCH_NOFFSET);
		if (timestamp_offset) {
			struct work *base_work = work;

			work = copy_work_noffset(base_work, timestamp_offset);
			free_work(base_work);
		}

		/* Test against the difficulty we asked for along with the work */
		wdiff = bits_to_diff(wdiffbits);
		hashes = (uint64_t)wdiff * 0x100000000ull;
		ret = true; // TODO: test_nonce_diff(work, nonce, wdiff);

		if (opt_debug) {
			/* Debugging, remove me */
			swab256(rhash, work->hash);
			bin2hex(outhash, rhash, 8);
			applog(LOG_DEBUG, "submit work %s 0x%04x 0x%08x %d 0x%08x",
			       outhash, retwork, mcu_tag, timestamp_offset, nonce);
		}
		
		hashes_done2(corethr, hashes, NULL);

		if (likely(ret)) {
			coreno = asic * 4 + core;
			if (unlikely(asic > 1 || core > 3 || pipe > 127 || pipeno > 1023)) {
				applog(LOG_WARNING, "%s %d: MCU invalid pipe asic %d core %d pipe %d",
				       cointerra->drv->name, cointerra->device_id, asic, core, pipe);
				coreno = 0;
			} else {
				info->last_pipe_nonce[pipeno] = time(NULL);
				bitchar = pipeno / 8;
				bitbit = pipeno % 8;
				info->pipe_bitmap[bitchar] |= 0x80 >> bitbit;
			}

			applog(LOG_DEBUG, "%"PRIpreprv": Submitting tested work job_id %s work_id %u",
			       corecgpu->proc_repr, work->job_id, work->subid);
			ret = submit_nonce(corethr, work, nonce);

			mutex_lock(&info->lock);
			info->share_hashes += hashes;
			info->tot_core_hashes[coreno] += hashes;
			info->hashes += nonce;
			mutex_unlock(&info->lock);
		} else {
			char sendbuf[CTA_MSG_SIZE];

			applog(LOG_DEBUG, "%s %d: Notify bad match work",
			       cointerra->drv->name, cointerra->device_id);
			if (opt_debug) {
				unsigned char midstate[32], wdata[12];
				char hexmidstate[68], hexwdata[28];
				uint16_t wid;

				memcpy(&wid, &info->work_id, 2);
				flip32(midstate, work->midstate);
				bin2hex(hexmidstate, midstate, 32);
				flip12(wdata, &work->data[64]);
				bin2hex(hexwdata, wdata, 12);
				applog(LOG_DEBUG, "False match sent: work id %u midstate %s  blkhdr %s",
				       wid, hexmidstate, hexwdata);
				applog(LOG_DEBUG, "False match reports: work id 0x%04x MCU id 0x%08x work diff %.1f",
				       retwork, mcu_tag, wdiff);
				applog(LOG_DEBUG, "False match tested: nonce 0x%08x noffset %d %s",
				       nonce, timestamp_offset, outhash);
			}

			/* Tell the device we got a false match */
			cta_gen_message(sendbuf, CTA_SEND_FMATCH);
			memcpy(sendbuf + 3, buf, CTA_MSG_SIZE - 3);
			cta_send_msg(cointerra, sendbuf);
		}
		free_work(work);
	} else {
		applog(LOG_INFO, "%s %d: Matching work id 0x%X %d not found", cointerra->drv->name,
		       cointerra->device_id, retwork, __LINE__);
		inc_hw_errors3(corethr, NULL, &nonce, bits_to_diff(wdiffbits));

		mutex_lock(&info->lock);
		info->no_matching_work++;
		mutex_unlock(&info->lock);
	}
}

static void cta_parse_wdone(struct thr_info *thr, struct cgpu_info *cointerra,
			    struct cointerra_info *info, char *buf)
{
	uint16_t retwork = *(uint16_t *)(&buf[CTA_DRIVER_TAG]);
	struct work *work = take_work_by_id(cointerra, retwork);
	uint64_t hashes;

	if (likely(work))
		free_work(work);
	else {
		applog(LOG_INFO, "%s %d: Done work not found id 0x%X %d",
		       cointerra->drv->name, cointerra->device_id, retwork, __LINE__);
		inc_hw_errors_only(thr);
	}

	/* Removing hashes from work done message */
	hashes = hu64_from_msg(buf, CTA_WDONE_NONCES);
	if (unlikely(hashes > (61 * 0x100000000ull))) {
		applog(LOG_INFO, "%s Invalid hash returned %"PRIu64"x %"PRIu64"x %"PRIu64"X",
		       __func__, info->hashes, hashes, hashes);
		hashes = 0;
	}

	mutex_lock(&info->lock);
	info->hashes += hashes;
	mutex_unlock(&info->lock);
}

static void u16array_from_msg(uint16_t *u16, int entries, int var, char *buf)
{
	int i, j;

	for (i = 0, j = 0; i < entries; i++, j += sizeof(uint16_t))
		u16[i] = hu16_from_msg(buf, var + j);
}

static void cta_parse_statread(struct cgpu_info *cointerra, struct cointerra_info *info,
			       char *buf)
{
	float max_temp = 0;
	int i;

	mutex_lock(&info->lock);
	u16array_from_msg(info->coretemp, CTA_CORES, CTA_STAT_CORETEMPS, buf);
	info->ambtemp_low = hu16_from_msg(buf, CTA_STAT_AMBTEMP_LOW);
	info->ambtemp_avg = hu16_from_msg(buf, CTA_STAT_AMBTEMP_AVG);
	info->ambtemp_high = hu16_from_msg(buf, CTA_STAT_AMBTEMP_HIGH);
	u16array_from_msg(info->pump_tachs, CTA_PUMPS, CTA_STAT_PUMP_TACHS, buf);
	u16array_from_msg(info->fan_tachs, CTA_FANS, CTA_STAT_FAN_TACHS, buf);
	u16array_from_msg(info->corevolts, CTA_CORES, CTA_STAT_CORE_VOLTS, buf);
	info->volts33 = hu16_from_msg(buf, CTA_STAT_VOLTS33);
	info->volts12 = hu16_from_msg(buf, CTA_STAT_VOLTS12);
	info->inactive = hu16_from_msg(buf, CTA_STAT_INACTIVE);
	info->active = hu16_from_msg(buf, CTA_STAT_ACTIVE);
	mutex_unlock(&info->lock);

	for (i = 0; i < CTA_CORES; i++) {
		if (info->coretemp[i] > max_temp)
			max_temp = info->coretemp[i];
	}
	max_temp /= 100.0;
	/* Store the max temperature in the cgpu struct as an exponentially
	 * changing value. */
	cointerra->temp = cointerra->temp * 0.63 + max_temp * 0.37;
}

static void u8array_from_msg(uint8_t *u8, int entries, int var, char *buf)
{
	int i;

	for (i = 0; i < entries; i++)
		u8[i] = u8_from_msg(buf, var + i);
}

static void cta_parse_statset(struct cointerra_info *info, char *buf)
{
	mutex_lock(&info->lock);
	u8array_from_msg(info->coreperf, CTA_CORES, CTA_STAT_PERFMODE, buf);
	u8array_from_msg(info->fanspeed, CTA_FANS, CTA_STAT_FANSPEEDS, buf);
	info->dies_active = u8_from_msg(buf, CTA_STAT_DIES_ACTIVE);
	u8array_from_msg(info->pipes_enabled, CTA_CORES, CTA_STAT_PIPES_ENABLED, buf);
	u16array_from_msg(info->corefreqs, CTA_CORES, CTA_STAT_CORE_FREQS, buf);
	info->uptime = hu32_from_msg(buf,CTA_STAT_UPTIME);
	mutex_unlock(&info->lock);
}

static void cta_parse_info(struct cgpu_info *cointerra, struct cointerra_info *info,
			   char *buf)
{
	mutex_lock(&info->lock);
	info->hwrev = hu64_from_msg(buf, CTA_INFO_HWREV);
	info->serial = hu32_from_msg(buf, CTA_INFO_SERNO);
	info->asics = u8_from_msg(buf, CTA_INFO_NUMASICS);
	info->dies = u8_from_msg(buf, CTA_INFO_NUMDIES);
	info->cores = hu16_from_msg(buf, CTA_INFO_NUMCORES);
	info->board_number = u8_from_msg(buf, CTA_INFO_BOARDNUMBER);
	info->fwrev[0] = u8_from_msg(buf, CTA_INFO_FWREV_MAJ);
	info->fwrev[1] = u8_from_msg(buf, CTA_INFO_FWREV_MIN);
	info->fwrev[2] = u8_from_msg(buf, CTA_INFO_FWREV_MIC);
	info->fw_year = hu16_from_msg(buf, CTA_INFO_FWDATE_YEAR);
	info->fw_month = u8_from_msg(buf, CTA_INFO_FWDATE_MONTH);
	info->fw_day = u8_from_msg(buf, CTA_INFO_FWDATE_DAY);
	info->init_diffbits = u8_from_msg(buf, CTA_INFO_INITDIFFBITS);
	info->min_diffbits = u8_from_msg(buf, CTA_INFO_MINDIFFBITS);
	info->max_diffbits = u8_from_msg(buf, CTA_INFO_MAXDIFFBITS);
	mutex_unlock(&info->lock);

#if 0
	if (!cointerra->unique_id) {
		uint32_t b32 = htobe32(info->serial);

		cointerra->unique_id = malloc((4 * 2) + 1);
		bin2hex(cointerra->unique_id, &b32, 4);
	}
#endif
}

static void cta_parse_rdone(struct cgpu_info *cointerra, struct cointerra_info *info,
			    char *buf)
{
	uint8_t reset_type, diffbits;
	uint64_t wdone;

	reset_type = buf[CTA_RESET_TYPE];
	diffbits = buf[CTA_RESET_DIFF];
	wdone = hu64_from_msg(buf, CTA_WDONE_NONCES);

	applog(LOG_INFO, "%s %d: Reset done type %u message %u diffbits %"PRIu64" done received",
	       cointerra->drv->name, cointerra->device_id, reset_type, diffbits, wdone);
	if (wdone) {
		applog(LOG_INFO, "%s %d: Reset done type %u message %u diffbits %"PRIu64" done received",
			cointerra->drv->name, cointerra->device_id, reset_type, diffbits, wdone);

		mutex_lock(&info->lock);
		info->hashes += wdone;
		mutex_unlock(&info->lock);
	}

	/* Note that the cgsem that is posted here must not be waited on while
	 * holding the info->lock to not get into a livelock since this
	 * function also grabs the lock first and it's always best to not sleep
	 * while holding a lock. */
	if (reset_type == CTA_RESET_NEW) {
		cta_clear_work(cointerra);
		/* Tell reset sender that the reset is complete
			* and it may resume. */
		notifier_wake(info->reset_notifier);
	}
}

static void cta_zero_stats(struct cgpu_info *cointerra);

static void cta_parse_debug(struct cointerra_info *info, char *buf)
{
	mutex_lock(&info->lock);

	info->tot_underruns = hu16_from_msg(buf, CTA_STAT_UNDERRUNS);
	u16array_from_msg(info->tot_hw_errors, CTA_CORES, CTA_STAT_HW_ERRORS, buf);
	info->tot_hashes = hu64_from_msg(buf, CTA_STAT_HASHES);
	info->tot_flushed_hashes = hu64_from_msg(buf, CTA_STAT_FLUSHED_HASHES);
	info->autovoltage = u8_from_msg(buf, CTA_STAT_AUTOVOLTAGE);
	info->current_ps_percent = u8_from_msg(buf, CTA_STAT_POWER_PERCENT);
	info->power_used = hu16_from_msg(buf,CTA_STAT_POWER_USED);
	info->power_voltage = hu16_from_msg(buf,CTA_STAT_VOLTAGE);
	info->ipower_used = hu16_from_msg(buf,CTA_STAT_IPOWER_USED);
	info->ipower_voltage = hu16_from_msg(buf,CTA_STAT_IVOLTAGE);
	info->power_temps[0] = hu16_from_msg(buf,CTA_STAT_PS_TEMP1);
	info->power_temps[1] = hu16_from_msg(buf,CTA_STAT_PS_TEMP2);

	mutex_unlock(&info->lock);

#if 0
	/* Autovoltage is positive only once at startup and eventually drops
	 * to zero. After that time we reset the stats since they're unreliable
	 * till then. */
	if (unlikely(!info->autovoltage_complete && !info->autovoltage)) {
		struct cgpu_info *cointerra = info->thr->cgpu;

		info->autovoltage_complete = true;
		cgtime(&cointerra->dev_start_tv);
		cta_zero_stats(cointerra);
		cointerra->total_mhashes = 0;
		cointerra->accepted = 0;
		cointerra->rejected = 0;
		cointerra->hw_errors = 0;
		cointerra->utility = 0.0;
		cointerra->last_share_pool_time = 0;
		cointerra->diff1 = 0;
		cointerra->diff_accepted = 0;
		cointerra->diff_rejected = 0;
		cointerra->last_share_diff = 0;
	}
#endif
}

static void cta_parse_msg(struct thr_info *thr, struct cgpu_info *cointerra,
			  struct cointerra_info *info, char *buf)
{
	switch (buf[CTA_MSG_TYPE]) {
		default:
		case CTA_RECV_UNUSED:
			applog(LOG_INFO, "%s %d: Unidentified message type %u",
			       cointerra->drv->name, cointerra->device_id, buf[CTA_MSG_TYPE]);
			break;
		case CTA_RECV_REQWORK:
			cta_parse_reqwork(cointerra, info, buf);
			break;
		case CTA_RECV_MATCH:
			cta_parse_recvmatch(thr, cointerra, info, buf);
			break;
		case CTA_RECV_WDONE:
			applog(LOG_DEBUG, "%s %d: Work done message received",
			       cointerra->drv->name, cointerra->device_id);
			cta_parse_wdone(thr, cointerra, info, buf);
			break;
		case CTA_RECV_STATREAD:
			applog(LOG_DEBUG, "%s %d: Status readings message received",
			       cointerra->drv->name, cointerra->device_id);
			cta_parse_statread(cointerra, info, buf);
			break;
		case CTA_RECV_STATSET:
			applog(LOG_DEBUG, "%s %d: Status settings message received",
			       cointerra->drv->name, cointerra->device_id);
			cta_parse_statset(info, buf);
			break;
		case CTA_RECV_INFO:
			applog(LOG_DEBUG, "%s %d: Info message received",
			       cointerra->drv->name, cointerra->device_id);
			cta_parse_info(cointerra, info, buf);
			break;
		case CTA_RECV_MSG:
			applog(LOG_NOTICE, "%s %d: MSG: %s",
			       cointerra->drv->name, cointerra->device_id, &buf[CTA_MSG_RECVD]);
			break;
		case CTA_RECV_RDONE:
			cta_parse_rdone(cointerra, info, buf);
			break;
		case CTA_RECV_STATDEBUG:
			cta_parse_debug(info, buf);
			break;
	}
}

static void *cta_recv_thread(void *arg)
{
	struct thr_info *thr = (struct thr_info *)arg;
	struct cgpu_info *cointerra = thr->cgpu;
	struct cointerra_info *info = cointerra->device_data;
	char threadname[24];
	int offset = 0;

	snprintf(threadname, 24, "cta_recv/%d", cointerra->device_id);
	RenameThread(threadname);

	while (likely(!cointerra->shutdown)) {
		char buf[CTA_READBUF_SIZE];
		int amount;

		if (unlikely(0))
		{
			applog(LOG_DEBUG, "%s %d: Device disappeared, disabling recv thread",
			       cointerra->drv->name, cointerra->device_id);
			break;
		}

		amount = usb_read(info->ep, buf + offset, CTA_MSG_SIZE);
		if (amount != CTA_MSG_SIZE && amount != 0) {
			applog(LOG_ERR, "%s: Read error %s, read %d",
			       cointerra->dev_repr, bfg_strerror(errno, BST_ERRNO), amount);
			break;
		}
		offset += amount;

		while (offset >= CTA_MSG_SIZE) {
			char *msg = mystrstr(buf, offset, cointerra_hdr);
			int begin;

			if (unlikely(!msg)) {
				applog(LOG_WARNING, "%s %d: No message header found, discarding buffer",
				       cointerra->drv->name, cointerra->device_id);
				inc_hw_errors_only(thr);
				/* Save the last byte in case it's the fist
				 * byte of a header. */
				begin = CTA_MSG_SIZE - 1;
				offset -= begin;
				memmove(buf, buf + begin, offset);
				continue;
			}

			if (unlikely(msg != buf)) {
				begin = msg - buf;
				applog(LOG_WARNING, "%s %d: Reads out of sync, discarding %d bytes",
				       cointerra->drv->name, cointerra->device_id, begin);
				inc_hw_errors_only(thr);
				offset -= begin;
				memmove(buf, msg, offset);
				if (offset < CTA_MSG_SIZE)
					break;
			}

			/* We have enough buffer for a full message, parse now */
			cta_parse_msg(thr, cointerra, info, msg);
			offset -= CTA_MSG_SIZE;
			if (offset > 0)
				memmove(buf, buf + CTA_MSG_SIZE, offset);
		}
	}

	return NULL;
}

static bool cta_send_msg(struct cgpu_info *cointerra, char *buf)
{
	struct cointerra_info *info = cointerra->device_data;
	int amount;

	/* Serialise usb writes to prevent overlap in case multiple threads
	 * send messages */
	mutex_lock(&info->sendlock);
	amount = usb_write(info->ep, buf, CTA_MSG_SIZE);
	mutex_unlock(&info->sendlock);

	if (unlikely(amount != CTA_MSG_SIZE)) {
		applog(LOG_ERR, "%s: Write error %s, wrote %d of %d",
		       cointerra->dev_repr, bfg_strerror(errno, BST_ERRNO), amount, CTA_MSG_SIZE);
		return false;
	}
	return true;
}

static bool cta_prepare(struct thr_info *thr)
{
	struct cgpu_info *cointerra = thr->cgpu;
	struct lowlevel_device_info * const llinfo = cointerra->device_data;
	struct cointerra_info *info = calloc(sizeof(struct cointerra_info), 1);
	char buf[CTA_MSG_SIZE];

	sleep(1);
	
	if (unlikely(!info))
		quit(1, "Failed to calloc info in cta_detect_one");
	cointerra->device_data = info;
	/* Nominally set a requested value when starting, preempting the need
	 * for a req-work message. */
	info->requested = CTA_MAX_QUEUE;

	if (!cointerra_open(llinfo, cointerra->dev_repr, &info->usbh, &info->ep))
		return false;
	
	info->thr = thr;
	mutex_init(&info->lock);
	mutex_init(&info->sendlock);
	if (unlikely(pthread_cond_init(&info->wake_cond, NULL)))
		quit(1, "Failed to create cta pthread cond");
	notifier_init(info->reset_notifier);
	if (pthread_create(&info->read_thr, NULL, cta_recv_thread, (void *)thr))
		quit(1, "Failed to create cta_recv_thread");

	/* Request a single status setting message */
	cta_gen_message(buf, CTA_SEND_REQUEST);
	msg_from_hu16(buf, CTA_REQ_MSGTYPE, CTA_RECV_STATSET);
	msg_from_hu16(buf, CTA_REQ_INTERVAL, 0);
	if (!cta_send_msg(cointerra, buf))
		return false;

	/* Request status debug messages every 60 seconds */
	cta_gen_message(buf, CTA_SEND_REQUEST);
	msg_from_hu16(buf, CTA_REQ_MSGTYPE, CTA_RECV_STATDEBUG);
	msg_from_hu16(buf, CTA_REQ_INTERVAL, 6000);
	if (!cta_send_msg(cointerra, buf))
		return false;

	cgtime(&info->core_hash_start);

	return true;
}

static void cta_send_reset(struct cgpu_info *cointerra, struct cointerra_info *info,
			   uint8_t reset_type, uint8_t diffbits);
static void cta_flush_work(struct cgpu_info *cointerra);

/* *_fill and *_scanwork are serialised wrt to each other */
static bool cta_fill(struct cgpu_info *cointerra)
{
	struct cointerra_info *info = cointerra->device_data;
	bool ret = true;
	struct timeval tv_now, tv_latest;
	char buf[CTA_MSG_SIZE];
	struct work *work = NULL;
	unsigned short nroll_limit;
	uint32_t swab[8];
	uint8_t diffbits;

	//applog(LOG_WARNING, "%s %d: cta_fill %d", cointerra->drv->name, cointerra->device_id,__LINE__);

	if (unlikely(info->thr->work_restart))
		cta_flush_work(cointerra);

	mutex_lock(&info->lock);
	if (!info->requested)
		goto out_unlock;
	work = get_queued(cointerra);
	if (unlikely(!work)) {
		ret = false;
		goto out_unlock;
	}
	if (--info->requested > 0)
		ret = false;

	/* It does not matter what endian this uint16_t is since it will be
	 * the same value on sending to the MC as returning in match/done. This
	 * will automatically wrap as a uint16_t. It cannot be zero for the MCU
	 * though. */
	if (unlikely(++info->work_id == 0))
		info->work_id = 1;
	work->subid = info->work_id;

	diffbits = diff_to_bits(work->nonce_diff);

	cta_gen_message(buf, CTA_SEND_WORK);

 	memcpy(buf + CTA_DRIVER_TAG, &info->work_id, 2);

	flip32(swab, work->midstate);
	memcpy(buf + CTA_WORK_MIDSTATE, swab, 32);

	flip12(swab, &work->data[64]);
	memcpy(buf + CTA_WORK_DATA, swab, 12);

	timer_set_now(&tv_now);
	timer_set_delay(&tv_latest, &tv_now, cointerra_latest_result_usecs);
	nroll_limit = max(0, work_ntime_range(work, &tv_now, &tv_latest, cointerra_desired_roll));
	
	nroll_limit = htole16(nroll_limit);
	memcpy(buf + CTA_WORK_NROLL, &nroll_limit, 2);

	memcpy(buf + CTA_WORK_DIFFBITS, &diffbits, 1);

out_unlock:
	mutex_unlock(&info->lock);

	if (work) {
		cgtime(&work->tv_work_start);
		applog(LOG_DEBUG, "%s %d: Sending work job_id %s work_id %u", cointerra->drv->name,
		       cointerra->device_id, work->job_id, work->subid);
		if (unlikely(!cta_send_msg(cointerra, buf))) {
			work_completed(cointerra, work);
			applog(LOG_INFO, "%s %d: Failed to send work",
			       cointerra->drv->name, cointerra->device_id);
			/* The device will fail after this */
		}
	}

	return ret;
}

static void cta_send_reset(struct cgpu_info *cointerra, struct cointerra_info *info,
			   uint8_t reset_type, uint8_t diffbits)
{
	char buf[CTA_MSG_SIZE];
	int ret, retries = 0;

	/* Clear any accumulated messages in case we've gotten out of sync. */
	notifier_reset(info->reset_notifier);
resend:
	cta_gen_message(buf, CTA_SEND_RESET);

	buf[CTA_RESET_TYPE] = reset_type;
	buf[CTA_RESET_LOAD] = opt_cta_load ? opt_cta_load : 255;
	buf[CTA_RESET_PSLOAD] = opt_ps_load;

	applog(LOG_INFO, "%s %d: Sending Reset type %u with diffbits %u", cointerra->drv->name,
	       cointerra->device_id, reset_type, diffbits);
	cta_send_msg(cointerra, buf);

	/* Wait for read thread to parse a reset message and signal us we may
	 * return to submitting other messages. Use a timeout in case we have
	 * a problem and the reset done message never returns. */
	if (reset_type == CTA_RESET_NEW) {
		ret = notifier_wait_us(info->reset_notifier, CTA_RESET_TIMEOUT * 1000);
		if (ret) {
			if (++retries < 3) {
				applog(LOG_INFO, "%s %d: Timed out waiting for reset done msg, retrying",
				       cointerra->drv->name, cointerra->device_id);
				goto resend;
			}
			applog(LOG_WARNING, "%s %d: Timed out waiting for reset done msg",
			       cointerra->drv->name, cointerra->device_id);
		}
		/* Good place to flush any work we have */
		flush_queue(cointerra);
	}
}

static void cta_update_work(struct cgpu_info *);

static void cta_flush_work(struct cgpu_info *cointerra)
{
	struct cointerra_info *info = cointerra->device_data;

	if (1)
		cta_update_work(cointerra);
	else
	{
	applog(LOG_INFO, "%s %d: cta_flush_work %d", cointerra->drv->name, cointerra->device_id,
	       __LINE__);
	cta_send_reset(cointerra, info, CTA_RESET_NEW, 0);
	}
	info->thr->work_restart = false;
}

static void cta_update_work(struct cgpu_info *cointerra)
{
	struct cointerra_info *info = cointerra->device_data;

	applog(LOG_INFO, "%s %d: Update work", cointerra->drv->name, cointerra->device_id);
	cta_send_reset(cointerra, info, CTA_RESET_UPDATE, 0);
}

static void cta_zero_corehashes(struct cointerra_info *info)
{
	int i;

	for (i = 0; i < CTA_CORES; i++)
		info->tot_core_hashes[i] = 0;
	cgtime(&info->core_hash_start);
}

/* Send per core hashrate calculations at regular intervals ~every 5 minutes */
static void cta_send_corehashes(struct cgpu_info *cointerra, struct cointerra_info *info,
				double corehash_time)
{
	uint16_t core_ghs[CTA_CORES];
	double k[CTA_CORES];
	char buf[CTA_MSG_SIZE];
	int i, offset;

	for (i = 0; i < CTA_CORES; i++) {
		k[i] = (double)info->tot_core_hashes[i] / ((double)32 * (double)0x100000000ull);
		k[i] = sqrt(k[i]) + 1;
		k[i] *= k[i];
		k[i] = k[i] * 32 * ((double)0x100000000ull / (double)1000000000) / corehash_time;
		core_ghs[i] = k[i];
	}
	cta_gen_message(buf, CTA_SEND_COREHASHRATE);
	offset = CTA_CORE_HASHRATES;
	for (i = 0; i < CTA_CORES; i++) {
		msg_from_hu16(buf, offset, core_ghs[i]);
		offset += 2; // uint16_t
	}
	cta_send_msg(cointerra, buf);
}

static int64_t cta_scanwork(struct thr_info *thr)
{
	struct cgpu_info *cointerra = thr->cgpu;
	struct cointerra_info *info = cointerra->device_data;
	double corehash_time;
	struct timeval now;
	int64_t hashes;

	hashes = 0;

	if (unlikely(0))
	{
		hashes = -1;
		goto out;
	}

	cgtime(&now);

	if (unlikely(thr->work_restart)) {
		applog(LOG_INFO, "%s %d: Flush work line %d",
		     cointerra->drv->name, cointerra->device_id,__LINE__);
		cta_flush_work(cointerra);
	} else {
		struct timespec abstime, tsdiff = {0, 500000000};
		time_t now_t;
		int i;

		timeval_to_spec(&abstime, &now);
		timeraddspec(&abstime, &tsdiff);

		/* Discard work that was started more than 5 minutes ago as
		 * a safety precaution backup in case the hardware failed to
		 * return a work done message for some work items. */
		age_queued_work(cointerra, 300.0);

		/* Each core should be 1.7MH so at max diff of 32 should
		 * average a share every ~80 seconds.Use this opportunity to
		 * unset the bits in any pipes that have not returned a valid
		 * nonce for over 30 full nonce ranges or 2400s. */
		now_t = time(NULL);
		for (i = 0; i < 1024; i++) {
			if (unlikely(now_t > info->last_pipe_nonce[i] + 2400)) {
				int bitchar = i / 8, bitbit = i % 8;

				info->pipe_bitmap[bitchar] &= ~(0x80 >> bitbit);
			}
		}

		/* Sleep for up to 0.5 seconds, waking if we need work or
		 * have received a restart message. */
		mutex_lock(&info->lock);
		pthread_cond_timedwait(&info->wake_cond, &info->lock, &abstime);
		mutex_unlock(&info->lock);

		if (thr->work_restart) {
			applog(LOG_INFO, "%s %d: Flush work line %d",
			       cointerra->drv->name, cointerra->device_id,__LINE__);
			cta_flush_work(cointerra);
		}
	}

	corehash_time = tdiff(&now, &info->core_hash_start);
	if (corehash_time > 300) {
		cta_send_corehashes(cointerra, info, corehash_time);
		cta_zero_corehashes(info);
	}

	mutex_lock(&info->lock);
	info->tot_share_hashes += info->share_hashes;
	info->tot_calc_hashes += info->hashes;
	info->hashes = info->share_hashes = 0;
	mutex_unlock(&info->lock);

	if (unlikely(0))
		hashes = -1;
out:
	return hashes;
}

/* This is used for a work restart. We don't actually perform the work restart
 * here but wake up the scanwork loop if it's waiting on the conditional so
 * that it can test for the restart message. */
static void cta_wake(struct cgpu_info *cointerra)
{
	struct cointerra_info *info = cointerra->device_data;

	mutex_lock(&info->lock);
	pthread_cond_signal(&info->wake_cond);
	mutex_unlock(&info->lock);
}

static void cta_shutdown(struct thr_info *thr)
{
	struct cgpu_info *cointerra = thr->cgpu;

	cta_close(cointerra);
}

static void cta_zero_stats(struct cgpu_info *cointerra)
{
	struct cointerra_info *info = cointerra->device_data;

	info->tot_calc_hashes = 0;
	info->tot_reset_hashes = info->tot_hashes;
	info->tot_share_hashes = 0;
	cta_zero_corehashes(info);
}

static int bits_set(char v)
{
	int c;

	for (c = 0; v; c++)
		v &= v - 1;
	return c;
}

static struct api_data *cta_api_stats(struct cgpu_info *cgpu)
{
	struct api_data *root = NULL;
	struct cointerra_info *info = cgpu->device_data;
	double dev_runtime = cgpu_runtime(cgpu);
	int i, asic, core, coreno = 0;
	struct timeval now;
	char bitmaphex[36];
	uint64_t ghs, val;
	char buf[64];

	/* Info data */
	root = api_add_uint16(root, "HW Revision", &info->hwrev, false);
	root = api_add_uint32(root, "Serial", &info->serial, false);
	root = api_add_uint8(root, "Asics", &info->asics, false);
	root = api_add_uint8(root, "Dies", &info->dies, false);
	root = api_add_uint16(root, "Cores", &info->cores, false);
	root = api_add_uint8(root, "Board number", &info->board_number, false);
	sprintf(buf, "%u.%u.%u", info->fwrev[0], info->fwrev[1], info->fwrev[2]);
	root = api_add_string(root, "FW Revision", buf, true);
	sprintf(buf, "%04u-%02u-%02u", info->fw_year, info->fw_month, info->fw_day);
	root = api_add_string(root, "FW Date", buf, true);
	root = api_add_uint8(root, "Init diffbits", &info->init_diffbits, false);
	root = api_add_uint8(root, "Min diffbits", &info->min_diffbits, false);
	root = api_add_uint8(root, "Max diffbits", &info->max_diffbits, false);

	/* Status readings */
	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "CoreTemp%d", i);
		root = api_add_int16(root, buf, &info->coretemp[i], false);
	}
	root = api_add_int16(root, "Ambient Low", &info->ambtemp_low, false);
	root = api_add_int16(root, "Ambient Avg", &info->ambtemp_avg, false);
	root = api_add_int16(root, "Ambient High", &info->ambtemp_high, false);
	for (i = 0; i < CTA_PUMPS; i++) {
		sprintf(buf, "PumpRPM%d", i);
		root = api_add_uint16(root, buf, &info->pump_tachs[i], false);
	}
	for (i = 0; i < CTA_FANS; i++) {
		sprintf(buf, "FanRPM%d", i);
		root = api_add_uint16(root, buf, &info->fan_tachs[i], false);
	}
	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "CoreFreqs%d", i);
		root = api_add_uint16(root, buf, &info->corefreqs[i], false);
	}

	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "CoreVolts%d", i);
		root = api_add_uint16(root, buf, &info->corevolts[i], false);
	}
	root = api_add_uint16(root, "Volts3.3", &info->volts33, false);
	root = api_add_uint16(root, "Volts12", &info->volts12, false);
	root = api_add_uint16(root, "Inactive", &info->inactive, false);
	root = api_add_uint16(root, "Active", &info->active, false);

	/* Status settings */
	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "CorePerfMode%d", i);
		root = api_add_uint8(root, buf, &info->coreperf[i], false);
	}
	for (i = 0; i < CTA_FANS; i++) {
		sprintf(buf, "FanSpeed%d", i);
		root = api_add_uint8(root, buf, &info->fanspeed[i], false);
	}
	root = api_add_uint8(root, "DiesActive", &info->dies_active, false);
	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "PipesEnabled%d", i);
		root = api_add_uint8(root, buf, &info->pipes_enabled[i], false);
	}

	/* Status debug */
	root = api_add_int(root, "Underruns", &info->tot_underruns, false);
	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "HWErrors%d", i);
		root = api_add_uint16(root, buf, &info->tot_hw_errors[i], false);
	}
	ghs = info->tot_calc_hashes / dev_runtime;
	root = api_add_uint64(root, "Calc hashrate", &ghs, true);
	ghs = (info->tot_hashes - info->tot_reset_hashes) / dev_runtime;
	root = api_add_uint64(root, "Hashrate", &ghs, true);
	ghs = info->tot_share_hashes / dev_runtime;
	root = api_add_uint64(root, "Share hashrate", &ghs, true);
	root = api_add_uint64(root, "Total calc hashes", &info->tot_calc_hashes, false);
	ghs = info->tot_hashes - info->tot_reset_hashes;
	root = api_add_uint64(root, "Total hashes", &ghs, true);
	root = api_add_uint64(root, "Total raw hashes", &info->tot_hashes, false);
	root = api_add_uint64(root, "Total share hashes", &info->tot_share_hashes, false);
	root = api_add_uint64(root, "Total flushed hashes", &info->tot_flushed_hashes, false);
	val = cgpu->diff_accepted * 0x100000000ull;
	root = api_add_uint64(root, "Accepted hashes", &val, true);
	ghs = val / dev_runtime;
	root = api_add_uint64(root, "Accepted hashrate", &ghs, true);
	val = cgpu->diff_rejected * 0x100000000ull;
	root = api_add_uint64(root, "Rejected hashes", &val, true);
	ghs = val / dev_runtime;
	root = api_add_uint64(root, "Rejected hashrate", &ghs, true);

	cgtime(&now);
	dev_runtime = tdiff(&now, &info->core_hash_start);
	if (dev_runtime < 1)
		dev_runtime = 1;
	for (i = 0; i < CTA_CORES; i++) {
		sprintf(buf, "Core%d hashrate", i);
		ghs = info->tot_core_hashes[i] / dev_runtime;
		root = api_add_uint64(root, buf, &ghs, true);
	}
	root = api_add_uint32(root, "Uptime",&info->uptime,false);
	for (asic = 0; asic < 2; asic++) {
		for (core = 0; core < 4; core++) {
			char bitmapcount[40], asiccore[12];
			int count = 0;

			sprintf(asiccore, "Asic%dCore%d", asic, core);
			bin2hex(bitmaphex, &info->pipe_bitmap[coreno], 16);
			for (i = coreno; i < coreno + 16; i++)
				count += bits_set(info->pipe_bitmap[i]);
			snprintf(bitmapcount, 40, "%d:%s", count, bitmaphex);
			root = api_add_string(root, asiccore, bitmapcount, true);
			coreno += 16;
		}
	}
	root = api_add_uint8(root, "AV", &info->autovoltage, false);
	root = api_add_uint8(root, "Power Supply Percent", &info->current_ps_percent, false);
	root = api_add_uint16(root, "Power Used", &info->power_used, false);
	root = api_add_uint16(root, "IOUT", &info->power_used, false);
	root = api_add_uint16(root, "VOUT", &info->power_voltage, false);
	root = api_add_uint16(root, "IIN", &info->ipower_used, false);
	root = api_add_uint16(root, "VIN", &info->ipower_voltage, false);
	root = api_add_uint16(root, "PSTemp1", &info->power_temps[0], false);
	root = api_add_uint16(root, "PSTemp2", &info->power_temps[1], false);

	return root;
}

struct device_drv cointerra_drv = {
	.dname = "cointerra",
	.name = "CTA",
	.lowl_match = cointerra_lowl_match,
	.lowl_probe = cointerra_lowl_probe,
	.thread_init = cta_prepare,
	.minerloop = hash_queued_work,
	.queue_full = cta_fill,
	// TODO .update_work = cta_update_work,
	.scanwork = cta_scanwork,
	.flush_work = cta_wake,
	.get_api_stats = cta_api_stats,
	.thread_shutdown = cta_shutdown,
	.zero_stats = cta_zero_stats,
};
