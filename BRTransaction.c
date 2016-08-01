//
//  BRTransaction.c
//
//  Created by Aaron Voisine on 8/31/15.
//  Copyright (c) 2015 breadwallet LLC
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "BRTransaction.h"
#include "BRKey.h"
#include "BRAddress.h"
#include "BRArray.h"
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#define TX_VERSION           0x00000001
#define TX_LOCKTIME          0x00000000
#define SIGHASH_ALL          0x01 // default, sign all of the outputs
#define SIGHASH_NONE         0x02 // sign none of the outputs, I don't care where the bitcoins go
#define SIGHASH_SINGLE       0x03 // sign one of the outputs, I don't care where the other outputs go
#define SIGHASH_ANYONECANPAY 0x80 // let other people add inputs, I don't care where the rest of the bitcoins come from

// returns a random number less than upperBound, for non-cryptographic use only
uint32_t BRRand(uint32_t upperBound)
{
    static int first = 1;
    uint32_t r;
    
    // seed = (((FNV_OFFSET xor time)*FNV_PRIME) xor pid)*FNV_PRIME
    if (first) srand((((0x811C9dc5 ^ (unsigned)time(NULL))*0x01000193) ^ (unsigned)getpid())*0x01000193);
    first = 0;
    if (upperBound == 0 || upperBound > BR_RAND_MAX) upperBound = BR_RAND_MAX;
    
    do { // to avoid modulo bias, find a rand value not less than 0x100000000 % upperBound
        r = rand();
    } while (r < ((0xffffffff - upperBound*2) + 1) % upperBound); // (((0xffffffff - x*2) + 1) % x) == (0x100000000 % x)

    return r % upperBound;
}

void BRTxInputSetAddress(BRTxInput *input, const char *address)
{
    assert(input != NULL);
    assert(address == NULL || BRAddressIsValid(address));
    if (input->script) array_free(input->script);
    input->script = NULL;
    input->scriptLen = 0;
    memset(input->address, 0, sizeof(input->address));

    if (address) {
        strncpy(input->address, address, sizeof(input->address) - 1);
        input->scriptLen = BRAddressScriptPubKey(NULL, 0, address);
        array_new(input->script, input->scriptLen);
        array_set_count(input->script, input->scriptLen);
        BRAddressScriptPubKey(input->script, input->scriptLen, address);
    }
}

void BRTxInputSetScript(BRTxInput *input, const uint8_t *script, size_t scriptLen)
{
    assert(input != NULL);
    assert(script != NULL || scriptLen == 0);
    if (input->script) array_free(input->script);
    input->script = NULL;
    input->scriptLen = 0;
    memset(input->address, 0, sizeof(input->address));
    
    if (script) {
        input->scriptLen = scriptLen;
        array_new(input->script, scriptLen);
        array_add_array(input->script, script, scriptLen);
        BRAddressFromScriptPubKey(input->address, sizeof(input->address), script, scriptLen);
    }
}

void BRTxInputSetSignature(BRTxInput *input, const uint8_t *signature, size_t sigLen)
{
    assert(input != NULL);
    assert(signature != NULL || sigLen == 0);
    if (input->signature) array_free(input->signature);
    input->signature = NULL;
    input->sigLen = 0;
    
    if (signature) {
        input->sigLen = sigLen;
        array_new(input->signature, sigLen);
        array_add_array(input->signature, signature, sigLen);
        if (! input->address[0]) BRAddressFromScriptSig(input->address, sizeof(input->address), signature, sigLen);
    }
}

static size_t _BRTxInputData(const BRTxInput *input, uint8_t *data, size_t dataLen)
{
    size_t off = 0;
    
    if (data && off + sizeof(UInt256) <= dataLen) memcpy(&data[off], &input->txHash, sizeof(UInt256)); // previous out
    off += sizeof(UInt256);
    if (data && off + sizeof(uint32_t) <= dataLen) set_u32le(&data[off], input->index);
    off += sizeof(uint32_t);
    off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), input->sigLen);
    if (data && off + input->sigLen <= dataLen) memcpy(&data[off], input->signature, input->sigLen); // scriptSig
    off += input->sigLen;
    if (data && off + sizeof(uint32_t) <= dataLen) set_u32le(&data[off], input->sequence);
    off += sizeof(uint32_t);
    return (! data || off <= dataLen) ? off : 0;
}

void BRTxOutputSetAddress(BRTxOutput *output, const char *address)
{
    assert(output != NULL);
    assert(address == NULL || BRAddressIsValid(address));
    if (output->script) array_free(output->script);
    output->script = NULL;
    output->scriptLen = 0;
    memset(output->address, 0, sizeof(output->address));

    if (address) {
        strncpy(output->address, address, sizeof(output->address));
        output->scriptLen = BRAddressScriptPubKey(NULL, 0, address);
        array_new(output->script, output->scriptLen);
        array_set_count(output->script, output->scriptLen);
        BRAddressScriptPubKey(output->script, output->scriptLen, address);
    }
}

void BRTxOutputSetScript(BRTxOutput *output, const uint8_t *script, size_t scriptLen)
{
    assert(output != NULL);
    assert(script != NULL || scriptLen == 0);
    if (output->script) array_free(output->script);
    output->script = NULL;
    output->scriptLen = 0;
    memset(output->address, 0, sizeof(output->address));

    if (script) {
        output->scriptLen = scriptLen;
        array_new(output->script, scriptLen);
        array_add_array(output->script, script, scriptLen);
        BRAddressFromScriptPubKey(output->address, sizeof(output->address), script, scriptLen);
    }
}

static size_t _BRTransactionOutputData(const BRTransaction *tx, uint8_t *data, size_t dataLen, size_t index)
{
    BRTxOutput *output;
    size_t i, off = 0;
    
    for (i = (index == SIZE_MAX ? 0 : index); i < tx->outCount && (index == SIZE_MAX || index == i); i++) {
        output = &tx->outputs[i];
        if (data && off + sizeof(uint64_t) <= dataLen) set_u64le(&data[off], output->amount);
        off += sizeof(uint64_t);
        off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), output->scriptLen);
        if (data && off + output->scriptLen <= dataLen) memcpy(&data[off], output->script, output->scriptLen);
        off += output->scriptLen;
    }
    
    return (! data || off <= dataLen) ? off : 0;
}

// writes the data that needs to be hashed and signed for the tx input at index
// an index of SIZE_MAX will write the entire signed transaction
// returns number of bytes written, or total len needed if data is NULL
static size_t _BRTransactionData(const BRTransaction *tx, uint8_t *data, size_t dataLen, size_t index, int hashType)
{
    BRTxInput input;
    int anyoneCanPay = (hashType & SIGHASH_ANYONECANPAY), sigHash = (hashType & 0x1f);
    size_t i, off = 0;
    
    if (anyoneCanPay && index >= tx->inCount) return 0;
    if (data && off + sizeof(uint32_t) <= dataLen) set_u32le(&data[off], tx->version); // tx version
    off += sizeof(uint32_t);
    
    if (! anyoneCanPay) {
        off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), tx->inCount);
        
        for (i = 0; i < tx->inCount; i++) { // inputs
            input = tx->inputs[i];
            
            if (index == i || (index == SIZE_MAX && ! input.signature)) {
                input.signature = input.script; // TODO: handle OP_CODESEPARATOR
                input.sigLen = input.scriptLen;
            }
            else if (index != SIZE_MAX) {
                input.sigLen = 0;
                if (sigHash == SIGHASH_NONE || sigHash == SIGHASH_SINGLE) input.sequence = 0;
            }
            
            off += _BRTxInputData(&input, (data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0));
        }
    }
    else {
        off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), 1);
        input = tx->inputs[index];
        input.signature = input.script; // TODO: handle OP_CODESEPARATOR
        input.sigLen = input.scriptLen;
        off += _BRTxInputData(&input, (data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0));
    }
    
    if (sigHash != SIGHASH_SINGLE && sigHash != SIGHASH_NONE) { // SIGHASH_ALL outputs
        off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), tx->outCount);
        off += _BRTransactionOutputData(tx, (data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), SIZE_MAX);
    }
    else if (sigHash == SIGHASH_SINGLE && index < tx->outCount) { // SIGHASH_SINGLE outputs
        off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), index + 1);
        
        for (i = 0; i < index; i++)  {
            if (data && off + sizeof(uint64_t) <= dataLen) set_u64le(&data[off], -1LL);
            off += sizeof(uint64_t);
            off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), 0);
        }
        
        off += _BRTransactionOutputData(tx, (data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), index);
    }
    else off += BRVarIntSet((data ? &data[off] : NULL), (off <= dataLen ? dataLen - off : 0), 0); //SIGHASH_NONE outputs
    
    if (data && off + sizeof(uint32_t) <= dataLen) set_u32le(&data[off], tx->lockTime); // locktime
    off += sizeof(uint32_t);
    
    if (index != SIZE_MAX) {
        if (data && off + sizeof(uint32_t) <= dataLen) set_u32le(&data[off], hashType); // hash type
        off += sizeof(uint32_t);
    }
    
    return (! data || off <= dataLen) ? off : 0;
}

// returns a newly allocated empty transaction that must be freed by calling BRTransactionFree()
BRTransaction *BRTransactionNew(void)
{
    BRTransaction *tx = calloc(1, sizeof(*tx));

    assert(tx != NULL);
    tx->version = TX_VERSION;
    array_new(tx->inputs, 1);
    array_new(tx->outputs, 2);
    tx->lockTime = TX_LOCKTIME;
    tx->blockHeight = TX_UNCONFIRMED;
    return tx;
}

// buf must contain a serialized tx
// retruns a transaction that must be freed by calling BRTransactionFree()
BRTransaction *BRTransactionParse(const uint8_t *buf, size_t bufLen)
{
    assert(buf != NULL || bufLen == 0);
    if (! buf) return NULL;
    
    int isSigned = 1;
    size_t i, off = 0, sLen = 0, len = 0;
    BRTransaction *tx = BRTransactionNew();
    BRTxInput *input;
    BRTxOutput *output;
    
    tx->version = (off + sizeof(uint32_t) <= bufLen) ? get_u32le(&buf[off]) : 0;
    off += sizeof(uint32_t);
    tx->inCount = BRVarInt(&buf[off], (off <= bufLen ? bufLen - off : 0), &len);
    off += len;
    array_set_count(tx->inputs, tx->inCount);
    
    for (i = 0; off <= bufLen && i < tx->inCount; i++) {
        input = &tx->inputs[i];
        input->txHash = (off + sizeof(UInt256) <= bufLen) ? get_u256(&buf[off]) : UINT256_ZERO;
        off += sizeof(UInt256);
        input->index = (off + sizeof(uint32_t) <= bufLen) ? get_u32le(&buf[off]) : 0;
        off += sizeof(uint32_t);
        sLen = BRVarInt(&buf[off], (off <= bufLen ? bufLen - off : 0), &len);
        off += len;
        
        if (off + sLen <= bufLen && BRAddressFromScriptPubKey(NULL, 0, &buf[off], sLen) > 0) {
            BRTxInputSetScript(input, &buf[off], sLen);
            isSigned = 0;
        }
        else if (off + sLen <= bufLen) BRTxInputSetSignature(input, &buf[off], sLen);
        
        off += sLen;
        input->sequence = (off + sizeof(uint32_t) <= bufLen) ? get_u32le(&buf[off]) : 0;
        off += sizeof(uint32_t);
    }
    
    tx->outCount = BRVarInt(&buf[off], (off <= bufLen ? bufLen - off : 0), &len);
    off += len;
    array_set_count(tx->outputs, tx->outCount);
    
    for (i = 0; off <= bufLen && i < tx->outCount; i++) {
        output = &tx->outputs[i];
        output->amount = (off + sizeof(uint64_t) <= bufLen) ? get_u64le(&buf[off]) : 0;
        off += sizeof(uint64_t);
        sLen = BRVarInt(&buf[off], (off <= bufLen ? bufLen - off : 0), &len);
        off += len;
        if (off + sLen <= bufLen) BRTxOutputSetScript(output, &buf[off], sLen);
        off += sLen;
    }
    
    tx->lockTime = (off + sizeof(uint32_t) <= bufLen) ? get_u32le(&buf[off]) : 0;
    off += sizeof(uint32_t);
    
    if (tx->inCount == 0 || off > bufLen) {
        BRTransactionFree(tx);
        tx = NULL;
    }
    else if (isSigned) BRSHA256_2(&tx->txHash, buf, off);
    
    return tx;
}

// returns number of bytes written to buf, or total len needed if buf is NULL
// (tx->blockHeight and tx->timestamp are not serialized)
size_t BRTransactionSerialize(const BRTransaction *tx, uint8_t *buf, size_t bufLen)
{
    assert(tx != NULL);
    assert(buf != NULL || bufLen == 0);
    return (tx) ? _BRTransactionData(tx, buf, bufLen, SIZE_MAX, SIGHASH_ALL) : 0;
}

// adds an input to tx
void BRTransactionAddInput(BRTransaction *tx, UInt256 txHash, uint32_t index, const uint8_t *script, size_t scriptLen,
                           const uint8_t *signature, size_t sigLen, uint32_t sequence)
{
    BRTxInput input = { txHash, index, "", NULL, 0, NULL, 0, sequence };

    assert(tx != NULL);
    assert(! UInt256IsZero(txHash));
    assert(script != NULL || scriptLen == 0);
    assert(signature != NULL || sigLen == 0);
    
    if (tx) {
        if (script) BRTxInputSetScript(&input, script, scriptLen);
        if (signature) BRTxInputSetSignature(&input, signature, sigLen);
        array_add(tx->inputs, input);
        tx->inCount = array_count(tx->inputs);
    }
}

// adds an output to tx
void BRTransactionAddOutput(BRTransaction *tx, uint64_t amount, const uint8_t *script, size_t scriptLen)
{
    BRTxOutput output = { "", amount, NULL, 0 };
    
    assert(tx != NULL);
    assert(script != NULL || scriptLen == 0);
    
    if (tx) {
        BRTxOutputSetScript(&output, script, scriptLen);
        array_add(tx->outputs, output);
        tx->outCount = array_count(tx->outputs);
    }
}

// shuffles order of tx outputs
void BRTransactionShuffleOutputs(BRTransaction *tx)
{
    assert(tx != NULL);
    
    for (uint32_t i = 0; tx && i + 1 < tx->outCount; i++) { // fischer-yates shuffle
        uint32_t j = i + BRRand((uint32_t)tx->outCount - i);
        BRTxOutput t;
        
        if (j != i) {
            t = tx->outputs[i];
            tx->outputs[i] = tx->outputs[j];
            tx->outputs[j] = t;
        }
    }
}

// size in bytes if signed, or estimated size assuming compact pubkey sigs
size_t BRTransactionSize(const BRTransaction *tx)
{
    BRTxInput *input;
    size_t size;

    assert(tx != NULL);
    size = (tx) ? 8 + BRVarIntSize(tx->inCount) + BRVarIntSize(tx->outCount) : 0;
    
    for (size_t i = 0; tx && i < tx->inCount; i++) {
        input = &tx->inputs[i];
        
        if (input->signature) {
            size += input->sigLen;
        }
        else size += TX_INPUT_SIZE;
    }
    
    for (size_t i = 0; tx && i < tx->outCount; i++) {
        size += tx->outputs[i].scriptLen;
    }
    
    return size;
}

// minimum transaction fee needed for tx to relay across the bitcoin network
uint64_t BRTransactionStandardFee(const BRTransaction *tx)
{
    assert(tx != NULL);
    return ((BRTransactionSize(tx) + 999)/1000)*TX_FEE_PER_KB;
}

// checks if all signatures exist, but does not verify them
int BRTransactionIsSigned(const BRTransaction *tx)
{
    assert(tx != NULL);
    
    for (size_t i = 0; tx && i < tx->inCount; i++) {
        if (! tx->inputs[i].signature) return 0;
    }

    return (tx) ? 1 : 0;
}

// adds signatures to any inputs with NULL signatures that can be signed with any keys
// returns true if tx is signed
int BRTransactionSign(BRTransaction *tx, BRKey keys[], size_t keysCount)
{
    BRAddress addrs[keysCount], address;
    size_t i, j;
    
    assert(tx != NULL);
    assert(keys != NULL || keysCount == 0);
    
    for (i = 0; tx && i < keysCount; i++) {
        if (! BRKeyAddress(&keys[i], addrs[i].s, sizeof(addrs[i]))) addrs[i] = BR_ADDRESS_NONE;
    }
    
    for (i = 0; tx && i < tx->inCount; i++) {
        BRTxInput *input = &tx->inputs[i];
        
        if (! BRAddressFromScriptPubKey(address.s, sizeof(address), input->script, input->scriptLen)) continue;
        j = 0;
        while (j < keysCount && ! BRAddressEq(&addrs[j], &address)) j++;
        if (j >= keysCount) continue;
        
        const uint8_t *elems[BRScriptElements(NULL, 0, input->script, input->scriptLen)];
        size_t elemsCount = BRScriptElements(elems, sizeof(elems)/sizeof(*elems), input->script, input->scriptLen);
        uint8_t data[_BRTransactionData(tx, NULL, 0, i, SIGHASH_ALL)];
        size_t dataLen = _BRTransactionData(tx, data, sizeof(data), i, SIGHASH_ALL);
        uint8_t sig[73], pubKey[65], script[1 + sizeof(sig) + 1 + sizeof(pubKey)];
        size_t sigLen, pkLen, scriptLen;
        UInt256 md = UINT256_ZERO;
        
        BRSHA256_2(&md, data, dataLen);
        sigLen = BRKeySign(&keys[j], sig, sizeof(sig) - 1, md);
        sig[sigLen++] = SIGHASH_ALL;
        pkLen = BRKeyPubKey(&keys[j], pubKey, sizeof(pubKey));
        
        if (elemsCount >= 2 && *elems[elemsCount - 2] == OP_EQUALVERIFY) { // pay-to-pubkey-hash
            scriptLen = BRScriptPushData(script, sizeof(script), sig, sigLen);
            scriptLen += BRScriptPushData(&script[scriptLen], sizeof(script) - scriptLen, pubKey, pkLen);
            BRTxInputSetSignature(input, script, scriptLen);
        }
        else { // pay-to-pubkey
            scriptLen = BRScriptPushData(script, sizeof(script), sig, sigLen);
            BRTxInputSetSignature(input, script, scriptLen);
        }
    }
    
    if (tx && BRTransactionIsSigned(tx)) {
        uint8_t data[_BRTransactionData(tx, NULL, 0, SIZE_MAX, 0)];
        size_t len = _BRTransactionData(tx, data, sizeof(data), SIZE_MAX, 0);
        
        BRSHA256_2(&tx->txHash, data, len);
        return 1;
    }
    else return 0;
}

// true if tx meets IsStandard() rules: https://bitcoin.org/en/developer-guide#standard-transactions
int BRTransactionIsStandard(BRTransaction *tx)
{
    int r = 1;
    
    // TODO: XXXX implement
    
    return r;
}

// frees memory allocated for tx
void BRTransactionFree(BRTransaction *tx)
{
    assert(tx != NULL);
    
    if (tx) {
        for (size_t i = 0; i < tx->inCount; i++) {
            BRTxInputSetScript(&tx->inputs[i], NULL, 0);
            BRTxInputSetSignature(&tx->inputs[i], NULL, 0);
        }

        for (size_t i = 0; i < tx->outCount; i++) {
            BRTxOutputSetScript(&tx->outputs[i], NULL, 0);
        }

        array_free(tx->outputs);
        array_free(tx->inputs);
        free(tx);
    }
}
