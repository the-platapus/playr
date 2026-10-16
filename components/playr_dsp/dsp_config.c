/*
 *  Playr output stage: JSON form of dsp_config_t
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "dsp_config.h"

static const char *channel_names[] = { NULL, "left", "right", "both" };

static bool fail(char *err, size_t err_size, const char *fmt, const char *what) {
	if (err && err_size) snprintf(err, err_size, fmt, what);
	return false;
}

// a number for both channels, or an array of one value per channel
static bool parse_pair(const cJSON *item, float *out) {
	if (cJSON_IsNumber(item)) {
		out[0] = out[1] = item->valuedouble;
		return true;
	}
	if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != DSP_CHANNELS) return false;
	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		const cJSON *v = cJSON_GetArrayItem(item, ch);
		if (!cJSON_IsNumber(v)) return false;
		out[ch] = v->valuedouble;
	}
	return true;
}

static bool parse_bool_pair(const cJSON *item, bool *out) {
	if (cJSON_IsBool(item)) {
		out[0] = out[1] = cJSON_IsTrue(item);
		return true;
	}
	if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != DSP_CHANNELS) return false;
	for (int ch = 0; ch < DSP_CHANNELS; ch++) {
		const cJSON *v = cJSON_GetArrayItem(item, ch);
		if (!cJSON_IsBool(v)) return false;
		out[ch] = cJSON_IsTrue(v);
	}
	return true;
}

static bool parse_band(const cJSON *item, dsp_band_t *band, char *err, size_t err_size) {
	const cJSON *field;

	if (!cJSON_IsObject(item)) return fail(err, err_size, "eq bands must be objects%s", "");

	band->q = 0.707f;			// Butterworth, as typed by people
	band->gain_db = 0;
	band->channels = DSP_CH_BOTH;

	cJSON_ArrayForEach(field, item) {
		const char *key = field->string;
		if (!strcmp(key, "type")) {
			if (!cJSON_IsString(field) || !dsp_filter_parse(field->valuestring, &band->type)) {
				return fail(err, err_size, "eq type must be peak, lowshelf, highshelf, lowpass or highpass%s", "");
			}
		} else if (!strcmp(key, "freq") || !strcmp(key, "q") || !strcmp(key, "gain")) {
			if (!cJSON_IsNumber(field)) return fail(err, err_size, "eq '%s' must be a number", key);
			float v = field->valuedouble;
			if (key[0] == 'f') band->freq = v;
			else if (key[0] == 'q') band->q = v;
			else band->gain_db = v;
		} else if (!strcmp(key, "ch")) {
			band->channels = 0;
			for (int i = 1; cJSON_IsString(field) && i <= DSP_CH_BOTH; i++) {
				if (!strcmp(field->valuestring, channel_names[i])) band->channels = i;
			}
			if (!band->channels) return fail(err, err_size, "eq ch must be left, right or both%s", "");
		} else {
			return fail(err, err_size, "unknown eq key '%s'", key);
		}
	}

	if (!cJSON_GetObjectItemCaseSensitive(item, "type") || !cJSON_GetObjectItemCaseSensitive(item, "freq")) {
		return fail(err, err_size, "eq bands need at least a type and a freq%s", "");
	}

	return true;
}

bool dsp_config_from_json(const char *json, dsp_config_t *cfg, char *err, size_t err_size) {
	dsp_config_t c;
	const cJSON *field;
	bool ok = true;

	dsp_config_default(&c);

	if (!json || !*json) {
		*cfg = c;
		return true;
	}

	cJSON *root = cJSON_Parse(json);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return fail(err, err_size, "dsp config is not a JSON object%s", "");
	}

	cJSON_ArrayForEach(field, root) {
		const char *key = field->string;
		if (!strcmp(key, "enabled") || !strcmp(key, "dither")) {
			if (!cJSON_IsBool(field)) ok = fail(err, err_size, "'%s' must be true or false", key);
			else if (key[0] == 'e') c.enabled = cJSON_IsTrue(field);
			else c.dither = cJSON_IsTrue(field);
		} else if (!strcmp(key, "role")) {
			if (!cJSON_IsString(field) || !dsp_role_parse(field->valuestring, &c.role)) {
				ok = fail(err, err_size, "role must be stereo, left, right, mono or sub%s", "");
			}
		} else if (!strcmp(key, "preamp") || !strcmp(key, "highpass") || !strcmp(key, "lowpass")) {
			if (!cJSON_IsNumber(field)) ok = fail(err, err_size, "'%s' must be a number", key);
			else if (key[0] == 'p') c.preamp_db = field->valuedouble;
			else if (key[0] == 'h') c.highpass_hz = field->valuedouble;
			else c.lowpass_hz = field->valuedouble;
		} else if (!strcmp(key, "gain") || !strcmp(key, "delay")) {
			if (!parse_pair(field, key[0] == 'g' ? c.gain_db : c.delay_ms)) {
				ok = fail(err, err_size, "'%s' must be a number or [left, right]", key);
			}
		} else if (!strcmp(key, "invert")) {
			if (!parse_bool_pair(field, c.invert)) ok = fail(err, err_size, "'%s' must be a bool or [left, right]", key);
		} else if (!strcmp(key, "eq")) {
			const cJSON *band;
			if (!cJSON_IsArray(field)) ok = fail(err, err_size, "eq must be an array%s", "");
			else if (cJSON_GetArraySize(field) > DSP_MAX_BANDS) ok = fail(err, err_size, "too many eq bands%s", "");
			else cJSON_ArrayForEach(band, field) {
				if (!(ok = parse_band(band, c.bands + c.band_count++, err, err_size))) break;
			}
		} else {
			ok = fail(err, err_size, "unknown key '%s'", key);
		}
		if (!ok) break;
	}

	cJSON_Delete(root);

	if (ok) {
		const char *problem = dsp_config_check(&c);
		if (problem) ok = fail(err, err_size, "%s", problem);
	}

	if (ok) *cfg = c;
	return ok;
}

// keep the stored text short and readable: 1.4 rather than 1.39999997615814
static cJSON *number(float v, int decimals) {
	double scale = pow(10, decimals);
	return cJSON_CreateNumber(round(v * scale) / scale);
}

static void add_pair(cJSON *root, const char *key, const float *v, int decimals) {
	if (v[0] == v[1]) {
		cJSON_AddItemToObject(root, key, number(v[0], decimals));
	} else {
		cJSON *array = cJSON_CreateArray();
		for (int ch = 0; ch < DSP_CHANNELS; ch++) cJSON_AddItemToArray(array, number(v[ch], decimals));
		cJSON_AddItemToObject(root, key, array);
	}
}

char *dsp_config_to_json(const dsp_config_t *cfg) {
	cJSON *root = cJSON_CreateObject();
	cJSON *eq = cJSON_CreateArray();

	cJSON_AddBoolToObject(root, "enabled", cfg->enabled);
	cJSON_AddStringToObject(root, "role", dsp_role_name(cfg->role));
	cJSON_AddItemToObject(root, "preamp", number(cfg->preamp_db, 2));
	cJSON_AddItemToObject(root, "highpass", number(cfg->highpass_hz, 2));
	cJSON_AddItemToObject(root, "lowpass", number(cfg->lowpass_hz, 2));

	for (int i = 0; i < cfg->band_count; i++) {
		const dsp_band_t *b = cfg->bands + i;
		cJSON *band = cJSON_CreateObject();
		cJSON_AddStringToObject(band, "type", dsp_filter_name(b->type));
		cJSON_AddItemToObject(band, "freq", number(b->freq, 2));
		cJSON_AddItemToObject(band, "q", number(b->q, 3));
		cJSON_AddItemToObject(band, "gain", number(b->gain_db, 2));
		if (b->channels != DSP_CH_BOTH) cJSON_AddStringToObject(band, "ch", channel_names[b->channels]);
		cJSON_AddItemToArray(eq, band);
	}
	cJSON_AddItemToObject(root, "eq", eq);

	add_pair(root, "gain", cfg->gain_db, 2);
	add_pair(root, "delay", cfg->delay_ms, 3);
	if (cfg->invert[0] == cfg->invert[1]) {
		cJSON_AddBoolToObject(root, "invert", cfg->invert[0]);
	} else {
		cJSON *array = cJSON_CreateArray();
		for (int ch = 0; ch < DSP_CHANNELS; ch++) cJSON_AddItemToArray(array, cJSON_CreateBool(cfg->invert[ch]));
		cJSON_AddItemToObject(root, "invert", array);
	}
	cJSON_AddBoolToObject(root, "dither", cfg->dither);

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}
