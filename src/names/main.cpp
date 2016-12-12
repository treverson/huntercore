// Copyright (c) 2014-2016 Daniel Kraft
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "names/main.h"

#include "chainparams.h"
#include "coins.h"
#include "consensus/validation.h"
#include "hash.h"
#include "dbwrapper.h"
#include "script/interpreter.h"
#include "script/names.h"
#include "txmempool.h"
#include "undo.h"
#include "util.h"
#include "utilstrencodings.h"
#include "validation.h"

/* ************************************************************************** */
/* CNameTxUndo.  */

void
CNameTxUndo::fromOldState (const valtype& nm, const CCoinsView& view)
{
  name = nm;
  isNew = !view.GetName (name, oldData);
}

void
CNameTxUndo::apply (CCoinsViewCache& view) const
{
  if (isNew)
    view.DeleteName (name);
  else
    view.SetName (name, oldData, true);
}

/* ************************************************************************** */
/* CNameMemPool.  */

uint256
CNameMemPool::getTxForName (const valtype& name) const
{
  NameTxMap::const_iterator mi;

  mi = mapNameRegs.find (name);
  if (mi != mapNameRegs.end ())
    {
      assert (mapNameUpdates.count (name) == 0);
      return mi->second;
    }

  mi = mapNameUpdates.find (name);
  if (mi != mapNameUpdates.end ())
    {
      assert (mapNameRegs.count (name) == 0);
      return mi->second;
    }

  return uint256 ();
}

void
CNameMemPool::addUnchecked (const uint256& hash, const CTxMemPoolEntry& entry)
{
  AssertLockHeld (pool.cs);

  if (entry.isNameNew ())
    {
      const valtype& newHash = entry.getNameNewHash ();
      const NameTxMap::const_iterator mit = mapNameNews.find (newHash);
      if (mit != mapNameNews.end ())
        assert (mit->second == hash);
      else
        mapNameNews.insert (std::make_pair (newHash, hash));
    }

  if (entry.isNameRegistration ())
    {
      const valtype& name = entry.getName ();
      assert (mapNameRegs.count (name) == 0);
      mapNameRegs.insert (std::make_pair (name, hash));
    }

  if (entry.isNameUpdate ())
    {
      const valtype& name = entry.getName ();
      assert (mapNameUpdates.count (name) == 0);
      mapNameUpdates.insert (std::make_pair (name, hash));
    }
}

void
CNameMemPool::remove (const CTxMemPoolEntry& entry)
{
  AssertLockHeld (pool.cs);

  if (entry.isNameRegistration ())
    {
      const NameTxMap::iterator mit = mapNameRegs.find (entry.getName ());
      assert (mit != mapNameRegs.end ());
      mapNameRegs.erase (mit);
    }
  if (entry.isNameUpdate ())
    {
      const NameTxMap::iterator mit = mapNameUpdates.find (entry.getName ());
      assert (mit != mapNameUpdates.end ());
      mapNameUpdates.erase (mit);
    }
}

void
CNameMemPool::removeConflicts (const CTransaction& tx,
                               std::vector<CTransactionRef>* removed)
{
  AssertLockHeld (pool.cs);

  if (!tx.IsNamecoin ())
    return;

  BOOST_FOREACH (const CTxOut& txout, tx.vout)
    {
      const CNameScript nameOp(txout.scriptPubKey);
      if (nameOp.isNameOp () && nameOp.getNameOp () == OP_NAME_FIRSTUPDATE)
        {
          const valtype& name = nameOp.getOpName ();
          const NameTxMap::const_iterator mit = mapNameRegs.find (name);
          if (mit != mapNameRegs.end ())
            {
              const CTxMemPool::txiter mit2 = pool.mapTx.find (mit->second);
              assert (mit2 != pool.mapTx.end ());
              pool.removeRecursive (mit2->GetTx (), removed);
            }
        }
    }
}

void
CNameMemPool::removeReviveConflicts (const std::set<valtype>& revived,
                                     std::vector<CTransactionRef>* removed)
{
  AssertLockHeld (pool.cs);

  BOOST_FOREACH (const valtype& name, revived)
    {
      LogPrint ("names", "revived: %s, mempool: %u\n",
                ValtypeToString (name).c_str (), mapNameRegs.count (name));

      const NameTxMap::const_iterator mit = mapNameRegs.find (name);
      if (mit != mapNameRegs.end ())
        {
          const CTxMemPool::txiter mit2 = pool.mapTx.find (mit->second);
          assert (mit2 != pool.mapTx.end ());
          pool.removeRecursive (mit2->GetTx (), removed);
        }
    }
}

void
CNameMemPool::check (const CCoinsView& coins) const
{
  AssertLockHeld (pool.cs);

  std::set<valtype> nameRegs;
  std::set<valtype> nameUpdates;
  BOOST_FOREACH (const CTxMemPoolEntry& entry, pool.mapTx)
    {
      const uint256 txHash = entry.GetTx ().GetHash ();
      if (entry.isNameNew ())
        {
          const valtype& newHash = entry.getNameNewHash ();
          const NameTxMap::const_iterator mit = mapNameNews.find (newHash);

          assert (mit != mapNameNews.end ());
          assert (mit->second == txHash);
        }

      if (entry.isNameRegistration ())
        {
          const valtype& name = entry.getName ();

          const NameTxMap::const_iterator mit = mapNameRegs.find (name);
          assert (mit != mapNameRegs.end ());
          assert (mit->second == txHash);

          assert (nameRegs.count (name) == 0);
          nameRegs.insert (name);

          CNameData data;
          if (coins.GetName (name, data))
            assert (data.isDead ());
        }

      if (entry.isNameUpdate ())
        {
          const valtype& name = entry.getName ();

          const NameTxMap::const_iterator mit = mapNameUpdates.find (name);
          assert (mit != mapNameUpdates.end ());
          assert (mit->second == txHash);

          assert (nameUpdates.count (name) == 0);
          nameUpdates.insert (name);

          CNameData data;
          if (!coins.GetName (name, data))
            assert (false);
          assert (!data.isDead ());
        }
    }

  assert (nameRegs.size () == mapNameRegs.size ());
  assert (nameUpdates.size () == mapNameUpdates.size ());

  /* Check that nameRegs and nameUpdates are disjoint.  They must be since
     a name can only be in either category, depending on whether it exists
     at the moment or not.  */
  BOOST_FOREACH (const valtype& name, nameRegs)
    assert (nameUpdates.count (name) == 0);
  BOOST_FOREACH (const valtype& name, nameUpdates)
    assert (nameRegs.count (name) == 0);
}

bool
CNameMemPool::checkTx (const CTransaction& tx) const
{
  AssertLockHeld (pool.cs);

  if (!tx.IsNamecoin ())
    return true;

  /* In principle, multiple name_updates could be performed within the
     mempool at once (building upon each other).  This is disallowed, though,
     since the current mempool implementation does not like it.  (We keep
     track of only a single update tx for each name.)  */

  BOOST_FOREACH (const CTxOut& txout, tx.vout)
    {
      const CNameScript nameOp(txout.scriptPubKey);
      if (!nameOp.isNameOp ())
        continue;

      switch (nameOp.getNameOp ())
        {
        case OP_NAME_NEW:
          {
            const valtype& newHash = nameOp.getOpHash ();
            std::map<valtype, uint256>::const_iterator mi;
            mi = mapNameNews.find (newHash);
            if (mi != mapNameNews.end () && mi->second != tx.GetHash ())
              return false;
            break;
          }

        case OP_NAME_FIRSTUPDATE:
          {
            const valtype& name = nameOp.getOpName ();
            if (registersName (name))
              return false;
            break;
          }

        case OP_NAME_UPDATE:
          {
            const valtype& name = nameOp.getOpName ();
            if (updatesName (name))
              return false;
            break;
          }

        default:
          assert (false);
        }
    }

  return true;
}

/* ************************************************************************** */

bool
CheckNameTransaction (const CTransaction& tx, unsigned nHeight,
                      const CCoinsView& view,
                      CValidationState& state, unsigned flags)
{
  const std::string strTxid = tx.GetHash ().GetHex ();
  const char* txid = strTxid.c_str ();
  const bool fMempool = (flags & SCRIPT_VERIFY_NAMES_MEMPOOL);

  /* Ignore historic bugs.  */
  CChainParams::BugType type;
  if (Params ().IsHistoricBug (tx.GetHash (), nHeight, type))
    return true;

  /* As a first step, try to locate inputs and outputs of the transaction
     that are name scripts.  At most one input and output should be
     a name operation.  */

  int nameIn = -1;
  CNameScript nameOpIn;
  CAmount nameAmountIn;
  CCoins coinsIn;
  for (unsigned i = 0; i < tx.vin.size (); ++i)
    {
      const COutPoint& prevout = tx.vin[i].prevout;
      CCoins coins;
      if (!view.GetCoins (prevout.hash, coins))
        return error ("%s: failed to fetch input coins for %s", __func__, txid);

      const CNameScript op(coins.vout[prevout.n].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameIn != -1)
            return state.Invalid (error ("%s: multiple name inputs into"
                                         " transaction %s", __func__, txid));
          nameIn = i;
          nameOpIn = op;
          nameAmountIn = coins.vout[prevout.n].nValue;
          coinsIn = coins;
        }
    }

  int nameOut = -1;
  CNameScript nameOpOut;
  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp ())
        {
          if (nameOut != -1)
            return state.Invalid (error ("%s: multiple name outputs from"
                                         " transaction %s", __func__, txid));
          nameOut = i;
          nameOpOut = op;
        }
    }

  /* Check that no name inputs/outputs are present for a non-Namecoin tx.
     If that's the case, all is fine.  For a Namecoin tx instead, there
     should be at least an output (for NAME_NEW, no inputs are expected).  */

  if (!tx.IsNamecoin ())
    {
      if (nameIn != -1)
        return state.Invalid (error ("%s: non-Namecoin tx %s has name inputs",
                                     __func__, txid));
      if (nameOut != -1)
        return state.Invalid (error ("%s: non-Namecoin tx %s at height %u"
                                     " has name outputs",
                                     __func__, txid, nHeight));

      return true;
    }

  assert (tx.IsNamecoin ());
  if (nameOut == -1)
    return state.Invalid (error ("%s: Namecoin tx %s has no name outputs",
                                 __func__, txid));

  /* Check locked amount.  This is only part of the full rules, though.
     Here, we enforce the minimum amount.  For name_update, we also
     check that the amount is always increasing (but below, since this
     is not true for name_firstupdate due to the prepared name tx logic).
     The actual minimum game fee is enforced when validating moves.  */
  if (tx.vout[nameOut].nValue < NAMENEW_COIN_AMOUNT)
    return state.Invalid (error ("%s: greedy name", __func__));

  /* Handle NAME_NEW now, since this is easy and different from the other
     operations.  */

  if (nameOpOut.getNameOp () == OP_NAME_NEW)
    {
      if (nameIn != -1)
        return state.Invalid (error ("CheckNameTransaction: NAME_NEW with"
                                     " previous name input"));

      if (nameOpOut.getOpHash ().size () != 20)
        return state.Invalid (error ("CheckNameTransaction: NAME_NEW's hash"
                                     " has wrong size"));

      return true;
    }

  /* Now that we have ruled out NAME_NEW, check that we have a previous
     name input that is being updated.  For Huntercoin, take the new-style
     name registration into account.  */

  assert (nameOpOut.isAnyUpdate ());
  if (nameOpOut.getNameOp () == OP_NAME_FIRSTUPDATE
        && nameOpOut.isNewStyleRegistration ())
    {
      if (nameIn != -1)
        return state.Invalid (error ("%s: new-style registration with"
                                     " name input", __func__));
    }
  else if (nameIn == -1)
    return state.Invalid (error ("CheckNameTransaction: update without"
                                 " previous name input"));

  const valtype& name = nameOpOut.getOpName ();
  if (name.size () > MAX_NAME_LENGTH)
    return state.Invalid (error ("CheckNameTransaction: name too long"));
  if (nameOpOut.getOpValue ().size () > MAX_VALUE_LENGTH)
    return state.Invalid (error ("CheckNameTransaction: value too long"));

  /* Process NAME_UPDATE next.  */

  if (nameOpOut.getNameOp () == OP_NAME_UPDATE)
    {
      if (tx.vout[nameOut].nValue < nameAmountIn)
        return state.Invalid (error ("%s: name amount decreased in tx %s",
                                     __func__,
                                     tx.GetHash ().GetHex ().c_str ()));

      if (!nameOpIn.isAnyUpdate ())
        return state.Invalid (error ("CheckNameTransaction: NAME_UPDATE with"
                                     " prev input that is no update"));

      if (name != nameOpIn.getOpName ())
        return state.Invalid (error ("%s: NAME_UPDATE name mismatch to prev tx"
                                     " found in %s", __func__, txid));

      /* Check that the name is existing and living.  This is redundant
         since the move validator also checks against the players
         in the game state, but it can't hurt to have the extra check here.  */
      CNameData oldName;
      if (!view.GetName (name, oldName))
        return state.Invalid (error ("%s: NAME_UPDATE name does not exist",
                                     __func__));
      if (oldName.isDead ())
        return state.Invalid (error ("%s: NAME_UPDATE name is dead", __func__));

      /* This is an internal consistency check.  If everything is fine,
         the input coins from the UTXO database should match the
         name database.  */
      assert (static_cast<unsigned> (coinsIn.nHeight) == oldName.getHeight ());
      assert (tx.vin[nameIn].prevout == oldName.getUpdateOutpoint ());

      return true;
    }

  /* Finally, NAME_FIRSTUPDATE.  Checks only necessary for the old-style
     name registration method.  */

  assert (nameOpOut.getNameOp () == OP_NAME_FIRSTUPDATE);
  if (!nameOpOut.isNewStyleRegistration ())
    {
      if (nameOpIn.getNameOp () != OP_NAME_NEW)
        return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                     " with non-NAME_NEW prev tx"));

      /* Maturity of NAME_NEW is checked only if we're not adding
         to the mempool.  */
      if (!fMempool)
        {
          assert (static_cast<unsigned> (coinsIn.nHeight) != MEMPOOL_HEIGHT);
          if (coinsIn.nHeight + MIN_FIRSTUPDATE_DEPTH > nHeight)
            return state.Invalid (error ("CheckNameTransaction: NAME_NEW"
                                         " is not mature for FIRST_UPDATE"));
        }

      if (nameOpOut.getOpRand ().size () > 20)
        return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                     " rand too large, %d bytes",
                                     nameOpOut.getOpRand ().size ()));

      {
        valtype toHash(nameOpOut.getOpRand ());
        toHash.insert (toHash.end (), name.begin (), name.end ());
        const uint160 hash = Hash160 (toHash);
        if (hash != uint160 (nameOpIn.getOpHash ()))
          return state.Invalid (error ("CheckNameTransaction: NAME_FIRSTUPDATE"
                                       " hash mismatch"));
      }
    }

  /* If the name exists already, check that it is dead.  This is redundant
     with the move validator, which also checks spawns against
     the players in the game state.  But the extra check won't hurt.  */
  CNameData oldName;
  if (view.GetName (name, oldName) && !oldName.isDead ())
    return state.Invalid (error ("%s: NAME_FIRSTUPDATE on a living name",
                                 __func__));

  /* We don't have to specifically check that miners don't create blocks with
     conflicting NAME_FIRSTUPDATE's, since the mining's CCoinsViewCache
     takes care of this with the check above already.  */

  return true;
}

void
ApplyNameTransaction (const CTransaction& tx, unsigned nHeight,
                      CCoinsViewCache& view, CBlockUndo& undo)
{
  assert (nHeight != MEMPOOL_HEIGHT);

  /* Handle historic bugs that should *not* be applied.  Names that are
     outputs should be marked as unspendable in this case.  Otherwise,
     we get an inconsistency between the UTXO set and the name database.  */
  CChainParams::BugType type;
  const uint256 txHash = tx.GetHash ();
  if (Params ().IsHistoricBug (txHash, nHeight, type)
      && type != CChainParams::BUG_FULLY_APPLY)
    {
      if (type == CChainParams::BUG_FULLY_IGNORE)
        {
          CCoinsModifier coins = view.ModifyCoins (txHash);
          for (unsigned i = 0; i < tx.vout.size (); ++i)
            {
              const CNameScript op(tx.vout[i].scriptPubKey);
              if (op.isNameOp () && op.isAnyUpdate ())
                {
                  if (!coins->IsAvailable (i) || !coins->Spend (i))
                    LogPrintf ("ERROR: %s : spending buggy name output failed",
                               __func__);
                }
            }
        }

      return;
    }

  /* This check must be done *after* the historic bug fixing above!  Some
     of the names that must be handled above are actually produced by
     transactions *not* marked as Namecoin tx.  */
  if (!tx.IsNamecoin ())
    return;

  /* Changes are encoded in the outputs.  We don't have to do any checks,
     so simply apply all these.  */

  for (unsigned i = 0; i < tx.vout.size (); ++i)
    {
      const CNameScript op(tx.vout[i].scriptPubKey);
      if (op.isNameOp () && op.isAnyUpdate ())
        {
          const valtype& name = op.getOpName ();
          LogPrint ("names", "Updating name at height %d: %s\n",
                    nHeight, ValtypeToString (name).c_str ());

          CNameTxUndo opUndo;
          opUndo.fromOldState (name, view);
          undo.vnameundo.push_back (opUndo);

          CNameData data;
          data.fromScript (nHeight, COutPoint (tx.GetHash (), i), op);
          view.SetName (name, data, false);
        }
    }
}

void
CheckNameDB (bool disconnect)
{
  const int option = GetArg ("-checknamedb", Params ().DefaultCheckNameDB ());

  if (option == -1)
    return;

  assert (option >= 0);
  if (option != 0)
    {
      if (disconnect || chainActive.Height () % option != 0)
        return;
    }

  pcoinsTip->Flush ();
  const bool ok = pcoinsTip->ValidateNameDB (*pgameDb);

  if (!ok)
    {
      LogPrintf ("ERROR: %s : name database is inconsistent\n", __func__);
      assert (false);
    }
}
