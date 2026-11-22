// Vectores de referencia hechos con el SDK oficial de JavaScript (@stellar/stellar-sdk).
// La prueba de tests/host arma lo mismo con el núcleo en C y compara byte a byte.
// Uso, en una carpeta donde esté instalado el SDK de JavaScript:
//   npm install @stellar/stellar-sdk
//   node vectores.mjs [archivo de salida]      (por defecto, vectores.h junto a este archivo)
import fs from 'node:fs';
import * as S from '@stellar/stellar-sdk';

const { Keypair, Networks, TransactionBuilder, Operation, Asset, Account, Memo, xdr, Address,
  nativeToScVal, hash, StrKey, SorobanDataBuilder } = S;

const SALIDA = process.argv[2] ?? new URL('./vectores.h', import.meta.url);
const RED = Networks.TESTNET;
const USDC_TESTNET = 'GBBD47IF6LWK7P7MDEVSCWR7DPUWV3NY3DTQEVFL4NAT4AQH3ZLLFLA5';
const USDC_MAINNET = 'GA5ZSEJYB37JRC5AVCIA5MOP4RHTM335X2KGX3IHOJAPP5RE34K4KZVN';

// Llaves fijas solo para los vectores (no se usan en la red).
const A = Keypair.fromRawEd25519Seed(Buffer.from(Array.from({ length: 32 }, (_, i) => i + 1)));
const B = Keypair.fromRawEd25519Seed(Buffer.from(Array.from({ length: 32 }, (_, i) => i + 0x21)));
const limites = { minTime: 0, maxTime: 1800000000 };
const cuenta = () => new Account(A.publicKey(), '123456788');

// 1. Pago clásico de XLM con memo numérico.
const tx1 = new TransactionBuilder(cuenta(), { fee: '100', networkPassphrase: RED, timebounds: limites })
  .addOperation(Operation.payment({ destination: B.publicKey(), asset: Asset.native(), amount: '1.2345678' }))
  .addMemo(Memo.id('4242'))
  .build();
tx1.sign(A);

// 2. Pago clásico de USDC, sin memo.
const tx2 = new TransactionBuilder(cuenta(), { fee: '100', networkPassphrase: RED, timebounds: limites })
  .addOperation(Operation.payment({ destination: B.publicKey(), asset: new Asset('USDC', USDC_TESTNET), amount: '2.5' }))
  .build();
tx2.sign(A);

// 3. Llamada al contrato del token: transfer(de, para, monto), con autorización de la cuenta de origen.
const contratoNativo = Asset.native().contractId(RED);
const args = [
  new Address(A.publicKey()).toScVal(),
  new Address(B.publicKey()).toScVal(),
  nativeToScVal(5000000n, { type: 'i128' }),
];
const llamada = new xdr.InvokeContractArgs({
  contractAddress: new Address(contratoNativo).toScAddress(),
  functionName: 'transfer',
  args,
});
const autorizacion = new xdr.SorobanAuthorizationEntry({
  credentials: xdr.SorobanCredentials.sorobanCredentialsSourceAccount(),
  rootInvocation: new xdr.SorobanAuthorizedInvocation({
    function: xdr.SorobanAuthorizedFunction.sorobanAuthorizedFunctionTypeContractFn(llamada),
    subInvocations: [],
  }),
});
const datosSoroban = new SorobanDataBuilder().setResourceFee(12345).setResources(100000, 200, 300).build();
const tx3 = new TransactionBuilder(cuenta(), { fee: '100', networkPassphrase: RED, timebounds: limites })
  .addOperation(Operation.invokeHostFunction({
    func: xdr.HostFunction.hostFunctionTypeInvokeContract(llamada),
    auth: [autorizacion],
  }))
  .setSorobanData(datosSoroban)
  .build();
tx3.sign(A);

// La misma llamada sin autorización ni datos de recursos: es lo que se manda a simular.
const tx3sim = new TransactionBuilder(cuenta(), { fee: '100', networkPassphrase: RED, timebounds: limites })
  .addOperation(Operation.invokeHostFunction({
    func: xdr.HostFunction.hostFunctionTypeInvokeContract(llamada),
    auth: [],
  }))
  .build();

// Valores de eventos: los dos formatos que permite SEP-41 para «transfer».
const eventoEntero = nativeToScVal(5000000n, { type: 'i128' }).toXDR('base64');
const eventoMapa = xdr.ScVal.scvMap([
  new xdr.ScMapEntry({ key: xdr.ScVal.scvSymbol('amount'), val: nativeToScVal(12345678n, { type: 'i128' }) }),
  new xdr.ScMapEntry({ key: xdr.ScVal.scvSymbol('to_muxed_id'), val: xdr.ScVal.scvU64(4242n) }),
]).toXDR('base64');

const bytes = (b) => '{' + Array.from(b).map((x) => '0x' + x.toString(16).padStart(2, '0')).join(',') + '}';
const texto = (s) => JSON.stringify(s);
const lineas = [
  '// Generado por tests/vectores/vectores.mjs con @stellar/stellar-sdk. No editar.',
  '#pragma once',
  '#include <stdint.h>',
  `static const char V_RED[] = ${texto(RED)};`,
  `static const uint8_t V_ID_RED[32] = ${bytes(hash(Buffer.from(RED)))};`,
  `static const uint8_t V_SEMILLA_A[32] = ${bytes(A.rawSecretKey())};`,
  `static const uint8_t V_PUB_A[32] = ${bytes(A.rawPublicKey())};`,
  `static const uint8_t V_PUB_B[32] = ${bytes(B.rawPublicKey())};`,
  `static const char V_CUENTA_A[] = ${texto(A.publicKey())};`,
  `static const char V_SECRETA_A[] = ${texto(A.secret())};`,
  `static const char V_CUENTA_B[] = ${texto(B.publicKey())};`,
  `static const char V_USDC_EMISOR[] = ${texto(USDC_TESTNET)};`,
  `static const char V_CONTRATO_NATIVO[] = ${texto(contratoNativo)};`,
  `static const char V_CONTRATO_USDC[] = ${texto(new Asset('USDC', USDC_TESTNET).contractId(RED))};`,
  `static const char V_TX1_HASH[] = ${texto(Buffer.from(tx1.hash()).toString('hex'))};`,
  `static const char V_TX1_SOBRE[] = ${texto(tx1.toEnvelope().toXDR('base64'))};`,
  `static const char V_TX2_SOBRE[] = ${texto(tx2.toEnvelope().toXDR('base64'))};`,
  `static const char V_TX3_SOBRE[] = ${texto(tx3.toEnvelope().toXDR('base64'))};`,
  `static const char V_TX3_SIM[] = ${texto(tx3sim.toEnvelope().toXDR('base64'))};`,
  `static const uint32_t V_TX3_COMISION = ${tx3.fee};`,
  `static const char V_DATOS_SOROBAN[] = ${texto(datosSoroban.toXDR('base64'))};`,
  `static const char V_AUTORIZACION[] = ${texto(autorizacion.toXDR('base64'))};`,
  `static const char V_TEMA_TRANSFER[] = ${texto(xdr.ScVal.scvSymbol('transfer').toXDR('base64'))};`,
  `static const char V_TEMA_CUENTA_B[] = ${texto(new Address(B.publicKey()).toScVal().toXDR('base64'))};`,
  `static const char V_CLAVE_CUENTA_A[] = ${texto(xdr.LedgerKey.account(new xdr.LedgerKeyAccount({ accountId: A.xdrAccountId() })).toXDR('base64'))};`,
  `static const char V_EVENTO_ENTERO[] = ${texto(eventoEntero)};`,
  `static const char V_EVENTO_MAPA[] = ${texto(eventoMapa)};`,
  `static const char V_SEP7[] = ${texto(`web+stellar:pay?destination=${B.publicKey()}&amount=2.5&asset_code=USDC&asset_issuer=${USDC_TESTNET}&memo=4242&memo_type=MEMO_ID`)};`,
  '',
];
fs.writeFileSync(SALIDA, lineas.join('\n'));

console.log('vectores escritos en', String(SALIDA));
console.log('contrato XLM testnet :', contratoNativo);
console.log('contrato USDC testnet:', new Asset('USDC', USDC_TESTNET).contractId(RED));
