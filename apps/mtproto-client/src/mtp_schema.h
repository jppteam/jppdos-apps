/*
 * mtp_schema.h — TL constructor and method ids.
 *
 * GENERATED, DO NOT HAND-EDIT. Produced from Telegram's official machine-
 * readable schema (https://core.telegram.org/schema/json) so that every value
 * here is exact rather than transcribed.
 *
 * Layer: 223
 *
 * These ids are layer-specific: a constructor gains a field and its id changes
 * with it. MTP_LAYER below is what initConnection announces, and it must stay
 * consistent with the ids in this file — regenerate the whole thing rather than
 * updating an entry by hand.
 *
 * The MTProto service-message ids (msg_container, rpc_result, bad_server_salt,
 * the handshake) are deliberately NOT here: they belong to the transport
 * protocol rather than the API layer, never change, and live next to the code
 * that uses them in mtp_rpc.c and mtp_auth.c.
 */
#pragma once

#define MTP_LAYER 223

/* ---- Connection wrapping ------------------------------------------------- */
#define MTP_ID_INVOKEWITHLAYER 0xda9b0d0du
#define MTP_ID_INITCONNECTION  0xc1cd5ea9u
#define MTP_ID_HELP_GETCONFIG  0xc4f9186bu

/* ---- Authorization ------------------------------------------------------- */
#define MTP_ID_AUTH_SENDCODE            0xa677244fu
#define MTP_ID_AUTH_RESENDCODE          0xcae47523u
#define MTP_ID_AUTH_SIGNIN              0x8d52a951u
#define MTP_ID_AUTH_SIGNUP              0xaac7b717u
#define MTP_ID_AUTH_CHECKPASSWORD       0xd18b4d16u
#define MTP_ID_AUTH_LOGOUT              0x3e72ba19u
#define MTP_ID_AUTH_EXPORTAUTHORIZATION 0xe5bfffcdu
#define MTP_ID_AUTH_IMPORTAUTHORIZATION 0xa57a7dadu
#define MTP_ID_ACCOUNT_GETPASSWORD      0x548a30f5u

/* ---- Dialogs, history and sending ---------------------------------------- */
#define MTP_ID_MESSAGES_GETDIALOGS  0xa0f4cb4fu
#define MTP_ID_MESSAGES_GETHISTORY  0x4423e6c5u
#define MTP_ID_MESSAGES_SENDMESSAGE 0x545cd15au
#define MTP_ID_MESSAGES_READHISTORY 0x0e306d3au
#define MTP_ID_MESSAGES_SETTYPING   0x58943ee2u
#define MTP_ID_USERS_GETUSERS       0x0d91a548u
#define MTP_ID_CONTACTS_GETCONTACTS 0x5dd69e12u

/* ---- Update state -------------------------------------------------------- */
#define MTP_ID_UPDATES_GETSTATE      0xedd4882au
#define MTP_ID_UPDATES_GETDIFFERENCE 0x19c2f763u

/* ---- Input peers and users ----------------------------------------------- */
#define MTP_ID_INPUTPEEREMPTY   0x7f3b18eau
#define MTP_ID_INPUTPEERSELF    0x7da07ec9u
#define MTP_ID_INPUTPEERCHAT    0x35a95cb9u
#define MTP_ID_INPUTPEERUSER    0xdde8a54cu
#define MTP_ID_INPUTPEERCHANNEL 0x27bcbbfcu
#define MTP_ID_INPUTUSERSELF    0xf7c1b13fu
#define MTP_ID_INPUTUSER        0xf21158c6u

/* ---- Login request payloads ---------------------------------------------- */
#define MTP_ID_CODESETTINGS            0xad253d78u
#define MTP_ID_INPUTCHECKPASSWORDEMPTY 0x9880f658u
#define MTP_ID_INPUTCHECKPASSWORDSRP   0xd27ff082u

/* ---- Typing actions ------------------------------------------------------ */
#define MTP_ID_SENDMESSAGETYPINGACTION 0x16bf744eu
#define MTP_ID_SENDMESSAGECANCELACTION 0xfd5ec8f5u

/* ---- Authorization results ----------------------------------------------- */
#define MTP_ID_AUTH_SENTCODE                    0x5e002502u
#define MTP_ID_AUTH_SENTCODESUCCESS             0x2390fe44u
#define MTP_ID_AUTH_AUTHORIZATION               0x2ea2c0d4u
#define MTP_ID_AUTH_AUTHORIZATIONSIGNUPREQUIRED 0x44747e9au
#define MTP_ID_AUTH_EXPORTEDAUTHORIZATION       0xb434e2b8u
#define MTP_ID_AUTH_LOGGEDOUT                   0xc3a2835fu

/* ---- How the login code was delivered ------------------------------------ */
#define MTP_ID_AUTH_SENTCODETYPEAPP         0x3dbb5986u
#define MTP_ID_AUTH_SENTCODETYPESMS         0xc000bba2u
#define MTP_ID_AUTH_SENTCODETYPECALL        0x5353e5a7u
#define MTP_ID_AUTH_SENTCODETYPEFLASHCALL   0xab03c6d9u
#define MTP_ID_AUTH_SENTCODETYPEMISSEDCALL  0x82006484u
#define MTP_ID_AUTH_SENTCODETYPEEMAILCODE   0xf450f59bu
#define MTP_ID_AUTH_SENTCODETYPEFRAGMENTSMS 0xd9565c39u
#define MTP_ID_AUTH_SENTCODETYPEFIREBASESMS 0x009fd736u
#define MTP_ID_AUTH_SENTCODETYPESMSWORD     0xa416ac81u
#define MTP_ID_AUTH_SENTCODETYPESMSPHRASE   0xb37794afu

/* ---- Two-factor password (SRP) ------------------------------------------- */
#define MTP_ID_ACCOUNT_PASSWORD                                                  0x957b50fbu
#define MTP_ID_PASSWORDKDFALGOUNKNOWN                                            0xd45ab096u
#define MTP_ID_PASSWORDKDFALGOSHA256SHA256PBKDF2HMACSHA512ITER100000SHA256MODPOW 0x3a912d4au

/* ---- Peers --------------------------------------------------------------- */
#define MTP_ID_PEERUSER    0x59511722u
#define MTP_ID_PEERCHAT    0x36c6019au
#define MTP_ID_PEERCHANNEL 0xa2a5371eu

/* ---- Users and chats ----------------------------------------------------- */
#define MTP_ID_USEREMPTY             0xd3bc4b7au
#define MTP_ID_USER                  0x31774388u
#define MTP_ID_CHATEMPTY             0x29562865u
#define MTP_ID_CHAT                  0x41cbf256u
#define MTP_ID_CHATFORBIDDEN         0x6592a1a7u
#define MTP_ID_CHANNEL               0x1c32b11cu
#define MTP_ID_CHANNELFORBIDDEN      0x17d493d5u
#define MTP_ID_USERPROFILEPHOTOEMPTY 0x4f11bae1u
#define MTP_ID_USERPROFILEPHOTO      0x82d1f706u

/* ---- User online status -------------------------------------------------- */
#define MTP_ID_USERSTATUSEMPTY     0x09d05049u
#define MTP_ID_USERSTATUSONLINE    0xedb93949u
#define MTP_ID_USERSTATUSOFFLINE   0x008c703fu
#define MTP_ID_USERSTATUSRECENTLY  0x7b197dc8u
#define MTP_ID_USERSTATUSLASTWEEK  0x541a1d1au
#define MTP_ID_USERSTATUSLASTMONTH 0x65899777u

/* ---- Dialogs ------------------------------------------------------------- */
#define MTP_ID_DIALOG                      0xd58a08c6u
#define MTP_ID_DIALOGFOLDER                0x71bd134cu
#define MTP_ID_MESSAGES_DIALOGS            0x15ba6c40u
#define MTP_ID_MESSAGES_DIALOGSSLICE       0x71e094f3u
#define MTP_ID_MESSAGES_DIALOGSNOTMODIFIED 0xf0e3e596u

/* ---- Messages ------------------------------------------------------------ */
#define MTP_ID_MESSAGEEMPTY                 0x90a6ca84u
#define MTP_ID_MESSAGE                      0x3ae56482u
#define MTP_ID_MESSAGESERVICE               0x7a800e0au
#define MTP_ID_MESSAGES_MESSAGES            0x1d73e7eau
#define MTP_ID_MESSAGES_MESSAGESSLICE       0x5f206716u
#define MTP_ID_MESSAGES_CHANNELMESSAGES     0xc776ba4eu
#define MTP_ID_MESSAGES_MESSAGESNOTMODIFIED 0x74535f21u
#define MTP_ID_MESSAGES_AFFECTEDMESSAGES    0x84d19185u

/* ---- Message media, for placeholder rendering ---------------------------- */
#define MTP_ID_MESSAGEMEDIAEMPTY       0x3ded6320u
#define MTP_ID_MESSAGEMEDIAPHOTO       0x695150d7u
#define MTP_ID_MESSAGEMEDIAGEO         0x56e0d474u
#define MTP_ID_MESSAGEMEDIACONTACT     0x70322949u
#define MTP_ID_MESSAGEMEDIAUNSUPPORTED 0x9f84f49eu
#define MTP_ID_MESSAGEMEDIADOCUMENT    0x52d8ccd9u
#define MTP_ID_MESSAGEMEDIAWEBPAGE     0xddf10c3bu
#define MTP_ID_MESSAGEMEDIAVENUE       0x2ec0533fu
#define MTP_ID_MESSAGEMEDIAGAME        0xfdb19008u
#define MTP_ID_MESSAGEMEDIAINVOICE     0xf6a548d3u
#define MTP_ID_MESSAGEMEDIAGEOLIVE     0xb940c666u
#define MTP_ID_MESSAGEMEDIAPOLL        0x4bd6e798u
#define MTP_ID_MESSAGEMEDIADICE        0x08cbec07u
#define MTP_ID_MESSAGEMEDIASTORY       0x68cb6283u
#define MTP_ID_MESSAGEMEDIAGIVEAWAY    0xaa073bebu

/* ---- Update containers --------------------------------------------------- */
#define MTP_ID_UPDATESTOOLONG         0xe317af7eu
#define MTP_ID_UPDATESHORTMESSAGE     0x313bc7f8u
#define MTP_ID_UPDATESHORTCHATMESSAGE 0x4d6deea5u
#define MTP_ID_UPDATESHORT            0x78d4dec1u
#define MTP_ID_UPDATESCOMBINED        0x725b04c3u
#define MTP_ID_UPDATES                0x74ae4240u

/* ---- Individual updates -------------------------------------------------- */
#define MTP_ID_UPDATENEWMESSAGE            0x1f2b0afdu
#define MTP_ID_UPDATENEWCHANNELMESSAGE     0x62ba04d9u
#define MTP_ID_UPDATEEDITMESSAGE           0xe40370a3u
#define MTP_ID_UPDATEEDITCHANNELMESSAGE    0x1b3f4df7u
#define MTP_ID_UPDATEDELETEMESSAGES        0xa20db0e5u
#define MTP_ID_UPDATEDELETECHANNELMESSAGES 0xc32d5b12u
#define MTP_ID_UPDATEREADHISTORYINBOX      0x9e84bc99u
#define MTP_ID_UPDATEREADHISTORYOUTBOX     0x2f2f21bfu
#define MTP_ID_UPDATEREADCHANNELINBOX      0x922e6e10u
#define MTP_ID_UPDATEREADCHANNELOUTBOX     0xb75f99a9u
#define MTP_ID_UPDATEUSERTYPING            0x2a17bf5cu
#define MTP_ID_UPDATECHATUSERTYPING        0x83487af0u
#define MTP_ID_UPDATECHANNELUSERTYPING     0x8c88c923u
#define MTP_ID_UPDATEUSERSTATUS            0xe5bdf8deu
#define MTP_ID_UPDATEUSERNAME              0xa7848924u

/* ---- Update difference --------------------------------------------------- */
#define MTP_ID_UPDATES_STATE             0xa56c2a3eu
#define MTP_ID_UPDATES_DIFFERENCEEMPTY   0x5d75a138u
#define MTP_ID_UPDATES_DIFFERENCE        0x00f49ca0u
#define MTP_ID_UPDATES_DIFFERENCESLICE   0xa8fb1981u
#define MTP_ID_UPDATES_DIFFERENCETOOLONG 0x4afe8f6du

/* ---- Config -------------------------------------------------------------- */
#define MTP_ID_CONFIG   0xcc1a241eu
#define MTP_ID_DCOPTION 0x18b7a10du

