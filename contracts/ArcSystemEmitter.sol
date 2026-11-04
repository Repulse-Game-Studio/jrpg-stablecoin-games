// SPDX-License-Identifier: MIT
pragma solidity 0.8.24;

/// Test stand-in for Arc's USDC system emitter (EIP-7708), placed at
/// 0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE with anvil_setCode. On Arc this address logs
/// an 18-decimal Transfer for every USDC move. Never deploy this anywhere real.
contract ArcSystemEmitter {
    event Transfer(address indexed from, address indexed to, uint256 value);

    /// Called by ArcMockUsdc so each ERC-20 transfer() logs here first, as on Arc.
    function emitTransfer(address from, address to, uint256 value) external {
        emit Transfer(from, to, value);
    }

    /// A plain native USDC send, as a wallet's "Send" button makes one: the value moves
    /// and the only log is this emitter's 18-decimal Transfer.
    function nativeSend(address payable to) external payable {
        (bool ok, ) = to.call{value: msg.value}("");
        require(ok, "send failed");
        emit Transfer(msg.sender, to, msg.value);
    }
}
