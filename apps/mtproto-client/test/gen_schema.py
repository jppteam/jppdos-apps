import json, sys, re

LAYER = 223
d = json.load(open('tl.json'))
M = {m['method']: int(m['id']) & 0xFFFFFFFF for m in d['methods']}
C = {c['predicate']: int(c['id']) & 0xFFFFFFFF for c in d['constructors']}

GROUPS = [
 ("Connection wrapping", "method", [
    "invokeWithLayer", "initConnection", "help.getConfig"]),
 ("Authorization", "method", [
    "auth.sendCode", "auth.resendCode", "auth.signIn", "auth.signUp",
    "auth.checkPassword", "auth.logOut",
    "auth.exportAuthorization", "auth.importAuthorization",
    "account.getPassword"]),
 ("Dialogs, history and sending", "method", [
    "messages.getDialogs", "messages.getHistory", "messages.sendMessage",
    "messages.readHistory", "messages.setTyping",
    "users.getUsers", "contacts.getContacts"]),
 ("Update state", "method", [
    "updates.getState", "updates.getDifference"]),

 ("Input peers and users", "ctor", [
    "inputPeerEmpty", "inputPeerSelf", "inputPeerChat", "inputPeerUser",
    "inputPeerChannel", "inputUserSelf", "inputUser"]),
 ("Login request payloads", "ctor", [
    "codeSettings", "inputCheckPasswordEmpty", "inputCheckPasswordSRP"]),
 ("Typing actions", "ctor", [
    "sendMessageTypingAction", "sendMessageCancelAction"]),

 ("Authorization results", "ctor", [
    "auth.sentCode", "auth.sentCodeSuccess", "auth.authorization",
    "auth.authorizationSignUpRequired", "auth.exportedAuthorization",
    "auth.loggedOut"]),
 ("How the login code was delivered", "ctor", [
    "auth.sentCodeTypeApp", "auth.sentCodeTypeSms", "auth.sentCodeTypeCall",
    "auth.sentCodeTypeFlashCall", "auth.sentCodeTypeMissedCall",
    "auth.sentCodeTypeEmailCode", "auth.sentCodeTypeFragmentSms",
    "auth.sentCodeTypeFirebaseSms", "auth.sentCodeTypeSmsWord",
    "auth.sentCodeTypeSmsPhrase"]),
 ("Two-factor password (SRP)", "ctor", [
    "account.password", "passwordKdfAlgoUnknown",
    "passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow"]),

 ("Peers", "ctor", ["peerUser", "peerChat", "peerChannel"]),
 ("Users and chats", "ctor", [
    "userEmpty", "user", "chatEmpty", "chat", "chatForbidden",
    "channel", "channelForbidden",
    "userProfilePhotoEmpty", "userProfilePhoto"]),
 ("User online status", "ctor", [
    "userStatusEmpty", "userStatusOnline", "userStatusOffline",
    "userStatusRecently", "userStatusLastWeek", "userStatusLastMonth"]),
 ("Dialogs", "ctor", [
    "dialog", "dialogFolder",
    "messages.dialogs", "messages.dialogsSlice", "messages.dialogsNotModified"]),
 ("Messages", "ctor", [
    "messageEmpty", "message", "messageService",
    "messages.messages", "messages.messagesSlice", "messages.channelMessages",
    "messages.messagesNotModified", "messages.affectedMessages"]),
 ("Message media, for placeholder rendering", "ctor", [
    "messageMediaEmpty", "messageMediaPhoto", "messageMediaGeo",
    "messageMediaContact", "messageMediaUnsupported", "messageMediaDocument",
    "messageMediaWebPage", "messageMediaVenue", "messageMediaGame",
    "messageMediaInvoice", "messageMediaGeoLive", "messageMediaPoll",
    "messageMediaDice", "messageMediaStory", "messageMediaGiveaway"]),
 ("Update containers", "ctor", [
    "updatesTooLong", "updateShortMessage", "updateShortChatMessage",
    "updateShort", "updatesCombined", "updates"]),
 ("Individual updates", "ctor", [
    "updateNewMessage", "updateNewChannelMessage", "updateEditMessage",
    "updateEditChannelMessage", "updateDeleteMessages",
    "updateDeleteChannelMessages", "updateReadHistoryInbox",
    "updateReadHistoryOutbox", "updateReadChannelInbox",
    "updateReadChannelOutbox", "updateUserTyping", "updateChatUserTyping",
    "updateChannelUserTyping", "updateUserStatus", "updateUserName"]),
 ("Update difference", "ctor", [
    "updates.state", "updates.differenceEmpty", "updates.difference",
    "updates.differenceSlice", "updates.differenceTooLong"]),
 ("Config", "ctor", ["config", "dcOption"]),
]

def cname(n):
    return "MTP_ID_" + re.sub(r'[^A-Z0-9]', '_', n.upper().replace('.', '_'))

out = []
missing = []
w = out.append
w("/*")
w(" * mtp_schema.h — TL constructor and method ids.")
w(" *")
w(" * GENERATED, DO NOT HAND-EDIT. Produced from Telegram's official machine-")
w(" * readable schema (https://core.telegram.org/schema/json) so that every value")
w(" * here is exact rather than transcribed.")
w(" *")
w(f" * Layer: {LAYER}")
w(" *")
w(" * These ids are layer-specific: a constructor gains a field and its id changes")
w(" * with it. MTP_LAYER below is what initConnection announces, and it must stay")
w(" * consistent with the ids in this file — regenerate the whole thing rather than")
w(" * updating an entry by hand.")
w(" *")
w(" * The MTProto service-message ids (msg_container, rpc_result, bad_server_salt,")
w(" * the handshake) are deliberately NOT here: they belong to the transport")
w(" * protocol rather than the API layer, never change, and live next to the code")
w(" * that uses them in mtp_rpc.c and mtp_auth.c.")
w(" */")
w("#pragma once")
w("")
w(f"#define MTP_LAYER {LAYER}")
w("")

for title, kind, names in GROUPS:
    tbl = M if kind == "method" else C
    w(f"/* ---- {title} " + "-" * max(3, 68 - len(title)) + " */")
    width = max(len(cname(n)) for n in names)
    for n in names:
        if n not in tbl:
            missing.append(n)
            continue
        w(f"#define {cname(n):<{width}} 0x{tbl[n]:08x}u")
    w("")

open('mtp_schema.h', 'w').write("\n".join(out) + "\n")
if missing:
    print("NOT IN SCHEMA (dropped):", ", ".join(missing), file=sys.stderr)
print(f"wrote {len(out)} lines")
