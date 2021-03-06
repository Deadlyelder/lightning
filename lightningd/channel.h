#ifndef LIGHTNING_LIGHTNINGD_CHANNEL_H
#define LIGHTNING_LIGHTNINGD_CHANNEL_H
#include "config.h"
#include <bitcoin/pubkey.h>
#include <bitcoin/shadouble.h>
#include <ccan/short_types/short_types.h>
#include <ccan/tal/tal.h>
#include <ccan/typesafe_cb/typesafe_cb.h>
#include <daemon/htlc.h>
#include <lightningd/channel_config.h>
#include <lightningd/derive_basepoints.h>
#include <lightningd/sphinx.h>
#include <stdbool.h>

struct signature;

/* View from each side */
struct channel_view {
	/* Current feerate in satoshis per 1000 weight. */
	u64 feerate_per_kw;

	/* What commitment number are we up to */
	u64 commitment_number;

	/* How much is owed to each side (includes pending changes) */
	u64 owed_msat[NUM_SIDES];
};

struct channel {
	/* Funding txid and output. */
	struct sha256_double funding_txid;
	unsigned int funding_txout;

	/* Keys used to spend funding tx. */
	struct pubkey funding_pubkey[NUM_SIDES];

	/* Millisatoshis in from commitment tx */
	u64 funding_msat;

	/* Who is paying fees. */
	enum side funder;

	/* Limits and settings on this channel. */
	const struct channel_config *config[NUM_SIDES];

	/* Basepoints for deriving keys. */
	struct basepoints basepoints[NUM_SIDES];

	/* Mask for obscuring the encoding of the commitment number. */
	u64 commitment_number_obscurer;

	/* All live HTLCs for this channel */
	struct htlc_map htlcs;

	/* What it looks like to each side. */
	struct channel_view view[NUM_SIDES];
};

/* Some requirements are self-specified (eg. my dust limit), others
 * are force upon the other side (eg. minimum htlc you can add).
 *
 * These values are also universally in msatsoshi.  These avoid
 * confusion: use them! */

/* BOLT #2:
 *
 * `dust-limit-satoshis` is the threshold below which output should be
 * generated for this node's commitment or HTLC transaction */
static inline u64 dust_limit_satoshis(const struct channel *channel,
				      enum side side)
{
	return channel->config[side]->dust_limit_satoshis;
}
/* BOLT #2:
 *
 * `max-htlc-value-in-inflight-msat` is a cap on total value of
 * outstanding HTLCs, which allows a node to limit its exposure to
 * HTLCs */
static inline u64 max_htlc_value_in_flight_msat(const struct channel *channel,
						enum side recipient)
{
	return channel->config[recipient]->max_htlc_value_in_flight_msat;
}
/* BOLT #2:
 *
 * similarly `max-accepted-htlcs` limits the number of outstanding
 * HTLCs the other node can offer. */
static inline u16 max_accepted_htlcs(const struct channel *channel,
				     enum side recipient)
{
	return channel->config[recipient]->max_accepted_htlcs;
}
/* BOLT #2:
 *
 * `channel-reserve-satoshis` is the minimum amount that the other
 * node is to keep as a direct payment. */
static inline u64 channel_reserve_msat(const struct channel *channel,
				       enum side side)
{
	return channel->config[!side]->channel_reserve_satoshis * 1000;
}
/* BOLT #2:
 *
 * `htlc-minimum-msat` indicates the smallest value HTLC this node will accept.
 */
static inline u32 htlc_minimum_msat(const struct channel *channel,
				    enum side recipient)
{
	return channel->config[recipient]->htlc_minimum_msat;
}
/* BOLT #2:
 *
 * `to-self-delay` is the number of blocks that the other nodes
 * to-self outputs must be delayed, using `OP_CHECKSEQUENCEVERIFY`
 * delays */
static inline u16 to_self_delay(const struct channel *channel, enum side side)
{
	return channel->config[!side]->to_self_delay;
}


/**
 * new_channel: Given initial fees and funding, what is initial state?
 * @ctx: tal context to allocate return value from.
 * @funding_txid: The commitment transaction id.
 * @funding_txout: The commitment transaction output number.
 * @funding_satoshis: The commitment transaction amount.
 * @push_msat: The amount the initator gives to the other side.
 * @feerate_per_kw: feerate per kiloweight (satoshis)
 * @local: local channel configuration
 * @remote: remote channel configuration
 * @local_basepoints: local basepoints.
 * @remote_basepoints: remote basepoints.
 * @local_fundingkey: local funding key
 * @remote_fundingkey: remote funding key
 * @funder: which side initiated it.
 *
 * Returns state, or NULL if malformed.
 */
struct channel *new_channel(const tal_t *ctx,
			    const struct sha256_double *funding_txid,
			    unsigned int funding_txout,
			    u64 funding_satoshis,
			    u64 push_msat,
			    u32 feerate_per_kw,
			    const struct channel_config *local,
			    const struct channel_config *remote,
			    const struct basepoints *local_basepoints,
			    const struct basepoints *remote_basepoints,
			    const struct pubkey *local_funding_pubkey,
			    const struct pubkey *remote_funding_pubkey,
			    enum side funder);

/**
 * channel_txs: Get the current commitment and htlc txs for the channel.
 * @ctx: tal context to allocate return value from.
 * @channel: The channel to evaluate
 * @htlc_map: Pointer to htlcs for each tx output (allocated off @ctx) or NULL.
 * @wscripts: Pointer to array of wscript for each tx returned (alloced off @ctx)
 * @per_commitment_point: Per-commitment point to determine keys
 * @side: which side to get the commitment transaction for
 *
 * Returns the unsigned commitment transaction for the committed state
 * for @side, followed by the htlc transactions in output order, and
 * fills in @htlc_map (if not NULL), or NULL on key derivation
 * failure.
 */
struct bitcoin_tx **channel_txs(const tal_t *ctx,
				const struct htlc ***htlcmap,
				const u8 ***wscripts,
				const struct channel *channel,
				const struct pubkey *per_commitment_point,
				enum side side);

/**
 * actual_feerate: what is the actual feerate for the local side.
 * @channel: The channel state
 * @theirsig: The other side's signature
 *
 * The fee calculated on a commitment transaction is a worst-case
 * approximation.  It's also possible that the desired feerate is not
 * met, because the initiator sets it while the other side is adding many
 * htlcs.
 *
 * This is the fee rate we actually care about, if we're going to check
 * whether it's actually too low.
 */
uint32_t actual_feerate(const struct channel *channel,
			const struct signature *theirsig);

/**
 * copy_channel: Make a deep copy of channel
 * @ctx: tal context to allocate return value from.
 * @channel: channel to copy.
 */
struct channel *copy_channel(const tal_t *ctx, const struct channel *channel);

enum channel_add_err {
	/* All OK! */
	CHANNEL_ERR_ADD_OK,
	/* Bad expiry value */
	CHANNEL_ERR_INVALID_EXPIRY,
	/* Not really a failure, if expected: it's an exact duplicate. */
	CHANNEL_ERR_DUPLICATE,
	/* Same ID, but otherwise different. */
	CHANNEL_ERR_DUPLICATE_ID_DIFFERENT,
	/* Would exceed the specified max_htlc_value_in_flight_msat */
	CHANNEL_ERR_MAX_HTLC_VALUE_EXCEEDED,
	/* Can't afford it */
	CHANNEL_ERR_CHANNEL_CAPACITY_EXCEEDED,
	/* HTLC is below htlc_minimum_msat */
	CHANNEL_ERR_HTLC_BELOW_MINIMUM,
	/* HTLC would push past max_accepted_htlcs */
	CHANNEL_ERR_TOO_MANY_HTLCS,
};

/**
 * channel_add_htlc: append an HTLC to channel if it can afford it
 * @channel: The channel
 * @offerer: the side offering the HTLC (to the other side).
 * @id: unique HTLC id.
 * @msatoshi: amount in millisatoshi.
 * @cltv_expiry: block number when HTLC can no longer be redeemed.
 * @payment_hash: hash whose preimage can redeem HTLC.
 * @routing: routing information (copied)
 *
 * If this returns CHANNEL_ERR_NONE, the fee htlc was added and
 * the output amounts adjusted accordingly.  Otherwise nothing
 * is changed.
 */
enum channel_add_err channel_add_htlc(struct channel *channel,
				      enum side sender,
				      u64 id,
				      u64 msatoshi,
				      u32 cltv_expiry,
				      const struct sha256 *payment_hash,
				      const u8 routing[TOTAL_PACKET_SIZE]);

/**
 * channel_get_htlc: find an HTLC
 * @channel: The channel
 * @offerer: the side offering the HTLC.
 * @id: unique HTLC id.
 */
struct htlc *channel_get_htlc(struct channel *channel, enum side sender, u64 id);

enum channel_remove_err {
	/* All OK! */
	CHANNEL_ERR_REMOVE_OK,
	/* No such HTLC. */
	CHANNEL_ERR_NO_SUCH_ID,
	/* Already have fulfilled it */
	CHANNEL_ERR_ALREADY_FULFILLED,
	/* Preimage doesn't hash to value. */
	CHANNEL_ERR_BAD_PREIMAGE,
	/* HTLC is not committed */
	CHANNEL_ERR_HTLC_UNCOMMITTED,
	/* HTLC is not committed and prior revoked on both sides */
	CHANNEL_ERR_HTLC_NOT_IRREVOCABLE
};

/**
 * channel_fail_htlc: remove an HTLC, funds to the side which offered it.
 * @channel: The channel state
 * @owner: the side who offered the HTLC (opposite to that failing it)
 * @id: unique HTLC id.
 *
 * This will remove the htlc and credit the value of the HTLC (back)
 * to its offerer.
 */
enum channel_remove_err channel_fail_htlc(struct channel *channel,
					  enum side owner, u64 id);

/**
 * channel_fulfill_htlc: remove an HTLC, funds to side which accepted it.
 * @channel: The channel state
 * @owner: the side who offered the HTLC (opposite to that fulfilling it)
 * @id: unique HTLC id.
 *
 * If the htlc exists, is not already fulfilled, the preimage is correct and
 * HTLC committed at the recipient, this will add a pending change to
 * remove the htlc and give the value of the HTLC to its recipient,
 * and return CHANNEL_ERR_FULFILL_OK.  Otherwise, it will return another error.
 */
enum channel_remove_err channel_fulfill_htlc(struct channel *channel,
					     enum side owner,
					     u64 id,
					     const struct preimage *preimage);

/**
 * approx_max_feerate: what's the we (initiator) could raise fee rate to?
 * @channel: The channel state
 *
 * This is not exact!  To check if their offer is valid, use can_afford_feerate.
 */
u64 approx_max_feerate(const struct channel *channel);

/**
 * can_afford_feerate: could the initiator pay for the fee at fee_rate?
 * @channel: The channel state
 * @feerate_per_kw: the new fee rate proposed
 */
bool can_afford_feerate(const struct channel *channel, u64 feerate_per_kw);

/**
 * adjust_fee: Change fee rate.
 * @channel: The channel state
 * @feerate_per_kw: fee in satoshi per 1000 bytes.
 * @side: which side to adjust.
 */
void adjust_fee(struct channel *channel, u64 feerate_per_kw, enum side side);

/**
 * force_fee: Change fees to a specific value.
 * @channel: The channel state
 * @fee: fee in satoshi.
 *
 * This is used for the close transaction, which specifies an exact fee.
 * If the fee cannot be paid in full, this return false (but cstate will
 * still be altered).
 */
bool force_fee(struct channel *channel, u64 fee);

/**
 * channel_sending_commit: commit all remote outstanding changes.
 * @channel: the channel
 *
 * This is where we commit to pending changes we've added; returns true if
 * anything changed for the remote side (if not, don't send!) */
bool channel_sending_commit(struct channel *channel);

/**
 * channel_rcvd_revoke_and_ack: accept ack on remote committed changes.
 * @channel: the channel
 * @oursfail: callback for any unfilfilled htlcs which are now fully removed.
 * @theirslocked: callback for any new htlcs which are now fully committed.
 * @cbarg: argument to pass through to @ourhtlcfail  & @theirhtlclocked
 *
 * This is where we commit to pending changes we've added; returns true if
 * anything changed for our local commitment (ie. we have pending changes).
 */
#define channel_rcvd_revoke_and_ack(channel, oursfail, theirslocked, cbarg) \
	channel_rcvd_revoke_and_ack_((channel),				\
				     typesafe_cb_preargs(void, void *,	\
							 (oursfail),	\
							 (cbarg),	\
							 const struct htlc *), \
				     typesafe_cb_preargs(void, void *,	\
							 (theirslocked), \
							 (cbarg),	\
							 const struct htlc *), \
				     (cbarg))

bool channel_rcvd_revoke_and_ack_(struct channel *channel,
				  void (*oursfail)(const struct htlc *htlc,
						   void *cbarg),
				  void (*theirslocked)(const struct htlc *htlc,
						       void *cbarg),
				  void *cbarg);

/**
 * channel_rcvd_commit: commit all local outstanding changes.
 * @channel: the channel
 * @theirsfulfilled: they are irrevocably committed to removal of htlc.
 * @cbarg: argument to pass through to @theirsfulfilled
 *
 * This is where we commit to pending changes we've added; returns true if
 * anything changed for our local commitment (ie. we had pending changes).
 * @theirsfulfilled is called for any HTLC we fulfilled which they are
 * irrevocably committed to, and is in our current commitment.
 */
#define channel_rcvd_commit(channel, theirsfulfilled, cbarg)		\
	channel_rcvd_commit_((channel),					\
			     typesafe_cb_preargs(void, void *,		\
						 (theirsfulfilled),	\
						 (cbarg),		\
						 const struct htlc *),	\
			     (cbarg))

bool channel_rcvd_commit_(struct channel *channel,
			  void (*theirsfulfilled)(const struct htlc *htlc,
						  void *cbarg),
			  void *cbarg);

/**
 * channel_sending_revoke_and_ack: sending ack on local committed changes.
 * @channel: the channel
 *
 * This is where we commit to pending changes we've added. Returns true if
 * anything changed for the remote commitment (ie. send a new commit).*/
bool channel_sending_revoke_and_ack(struct channel *channel);

/**
 * channel_awaiting_revoke_and_ack: are we waiting for revoke_and_ack?
 * @channel: the channel
 *
 * If true, we can't send a new commit message.
 */
bool channel_awaiting_revoke_and_ack(const struct channel *channel);
#endif /* LIGHTNING_DAEMON_CHANNEL_H */
