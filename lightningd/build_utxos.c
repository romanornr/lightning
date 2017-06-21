#include <bitcoin/base58.h>
#include <bitcoin/script.h>
#include <ccan/structeq/structeq.h>
#include <daemon/jsonrpc.h>
#include <lightningd/build_utxos.h>
#include <lightningd/lightningd.h>
#include <utils.h>
#include <wally_bip32.h>


/* FIXME: This is very slow with lots of inputs! */
static bool can_spend(struct lightningd *ld, const u8 *script,
		      u32 *index, bool *output_is_p2sh)
{
	struct ext_key ext;
	u64 bip32_max_index = db_get_intvar(ld->wallet->db, "bip32_max_index", 0);
	u32 i;

	/* If not one of these, can't be for us. */
	if (is_p2sh(script))
		*output_is_p2sh = true;
	else if (is_p2wpkh(script))
		*output_is_p2sh = false;
	else
		return false;

	for (i = 0; i < bip32_max_index; i++) {
		u8 *s;

		if (bip32_key_from_parent(ld->bip32_base, i,
					  BIP32_FLAG_KEY_PUBLIC, &ext)
		    != WALLY_OK) {
			abort();
		}
		s = scriptpubkey_p2wpkh_derkey(ld, ext.pub_key);
		if (*output_is_p2sh) {
			u8 *p2sh = scriptpubkey_p2sh(ld, s);
			tal_free(s);
			s = p2sh;
		}
		if (scripteq(s, script)) {
			tal_free(s);
			*index = i;
			return true;
		}
		tal_free(s);
	}
	return false;
}

static void json_addfunds(struct command *cmd,
			  const char *buffer, const jsmntok_t *params)
{
	struct lightningd *ld = ld_from_dstate(cmd->dstate);
	struct json_result *response = new_json_result(cmd);
	jsmntok_t *txtok;
	struct bitcoin_tx *tx;
	int output;
	size_t txhexlen, num_utxos = 0;
	u64 total_satoshi = 0;

	if (!json_get_params(buffer, params, "tx", &txtok, NULL)) {
		command_fail(cmd, "Need tx sending to address from newaddr");
		return;
	}

	txhexlen = txtok->end - txtok->start;
	tx = bitcoin_tx_from_hex(cmd, buffer + txtok->start, txhexlen);
	if (!tx) {
		command_fail(cmd, "'%.*s' is not a valid transaction",
			     txtok->end - txtok->start,
			     buffer + txtok->start);
		return;
	}

	/* Find an output we know how to spend. */
	for (output = 0; output < tal_count(tx->output); output++) {
		struct utxo *utxo;
		u32 index;
		bool is_p2sh;

		if (!can_spend(ld, tx->output[output].script, &index, &is_p2sh))
			continue;

		utxo = tal(ld, struct utxo);
		utxo->keyindex = index;
		utxo->is_p2sh = is_p2sh;
		utxo->amount = tx->output[output].amount;
		utxo->status = output_state_available;
		bitcoin_txid(tx, &utxo->txid);
		utxo->outnum = output;
		if (!wallet_add_utxo(ld->wallet, utxo, p2sh_wpkh)) {
			command_fail(cmd, "Could add outputs to wallet");
			tal_free(utxo);
			return;
		}
		total_satoshi += utxo->amount;
		num_utxos++;
	}

	if (!num_utxos) {
		command_fail(cmd, "No usable outputs");
		return;
	}

	json_object_start(response, NULL);
	json_add_num(response, "outputs", num_utxos);
	json_add_u64(response, "satoshis", total_satoshi);
	json_object_end(response);
	command_success(cmd, response);
}

static const struct json_command addfunds_command = {
	"addfunds",
	json_addfunds,
	"Add funds for lightningd to spend to create channels, using {tx}",
	"Returns how many {outputs} it can use and total {satoshis}"
};
AUTODATA(json_command, &addfunds_command);

const struct utxo **build_utxos(const tal_t *ctx,
				struct lightningd *ld, u64 satoshi_out,
				u32 feerate_per_kw, u64 dust_limit,
				u64 *change_satoshis, u32 *change_keyindex)
{
	u64 fee_estimate = 0;
	u64 bip32_max_index = db_get_intvar(ld->wallet->db, "bip32_max_index", 0);
	const struct utxo **utxos =
	    wallet_select_coins(ctx, ld->wallet, satoshi_out, feerate_per_kw,
				&fee_estimate, change_satoshis);

	/* Oops, didn't have enough coins available */
	if (!utxos)
		return NULL;

	/* Do we need a change output? */
	if (*change_satoshis < dust_limit) {
		*change_satoshis = 0;
		*change_keyindex = 0;
	} else {
		*change_keyindex = bip32_max_index + 1;
		db_set_intvar(ld->wallet->db, "bip32_max_index", *change_keyindex);
	}
	return utxos;
}
