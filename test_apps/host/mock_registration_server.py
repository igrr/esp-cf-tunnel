#!/usr/bin/env python3
"""Mock of the edge's RegistrationServer, built on the reference Cap'n Proto
RPC implementation (pycapnp). rpc_test connects to it to check that the
hand-written RPC client produces messages the reference implementation accepts.

registerConnection checks the parameters sent by rpc_test and answers with
connectionDetails, or with a ConnectionError when the account tag is "fail-me".

Run: uv run --with pycapnp test_apps/host/mock_registration_server.py SCHEMA_DIR PORT
"""

import asyncio
import sys

import capnp

schema_dir, port = sys.argv[1], int(sys.argv[2])
tunnelrpc = capnp.load(f"{schema_dir}/tunnelrpc.capnp", imports=[schema_dir])

EXPECTED_TUNNEL_ID = bytes.fromhex("e9b6087fd1554ccabb1b07a77bfc4306")


def check(cond, what):
    if not cond:
        print(f"MOCK CHECK FAILED: {what}", flush=True)
        raise ValueError(what)


class Registration(tunnelrpc.RegistrationServer.Server):
    async def registerConnection(self, auth, tunnelId, connIndex, options, _context, **kwargs):
        opts = options
        client = opts.client
        print(f"registerConnection account={auth.accountTag} secret_len={len(auth.tunnelSecret)} "
              f"tunnel={tunnelId.hex()} connIndex={connIndex} clientId={client.clientId.hex()} "
              f"features={list(client.features)} version={client.version} arch={client.arch} "
              f"originLocalIp={opts.originLocalIp.hex()} replace={opts.replaceExisting} "
              f"prev={opts.numPreviousAttempts}", flush=True)
        check(auth.tunnelSecret[:10] == bytes(range(1, 11)) and len(auth.tunnelSecret) == 32, "secret")
        check(tunnelId == EXPECTED_TUNNEL_ID, "tunnelId")
        check(connIndex == 2, "connIndex")
        check(client.clientId[:2] == b"\xaa\xbb" and len(client.clientId) == 16, "clientId")
        check(list(client.features) == ["serialized_headers", "allow_remote_config"], "features")
        check(client.version == "test/1.0" and client.arch == "esp32p4", "version/arch")
        check(opts.originLocalIp == bytes([192, 168, 1, 2]), "originLocalIp")
        check(not opts.replaceExisting and opts.numPreviousAttempts == 3, "options")

        result = _context.results.init("result")
        if auth.accountTag == "fail-me":
            err = result.result.init("error")
            err.cause = "mock refusal"
            err.retryAfter = 5_000_000_000
            err.shouldRetry = True
        else:
            check(auth.accountTag == "acct-tag", "accountTag")
            details = result.result.init("connectionDetails")
            details.uuid = bytes(range(0x10, 0x20))
            details.locationName = "mock01"
            details.tunnelIsRemotelyManaged = True

    async def unregisterConnection(self, _context, **kwargs):
        print("unregisterConnection", flush=True)

    async def updateLocalConfiguration(self, config, _context, **kwargs):
        print(f"updateLocalConfiguration {bytes(config)!r}", flush=True)


async def new_connection(stream):
    await capnp.TwoPartyServer(stream, bootstrap=Registration()).on_disconnect()


async def main():
    server = await capnp.AsyncIoStream.create_server(new_connection, "127.0.0.1", port)
    print(f"listening on {port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(capnp.run(main()))
