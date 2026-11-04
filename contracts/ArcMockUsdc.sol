// SPDX-License-Identifier: MIT
pragma solidity 0.8.24;

interface IArcSystemEmitter {
    function emitTransfer(address from, address to, uint256 value) external;
}

/// Test stand-in for Arc's USDC ERC-20 interface, placed at
/// 0x3600000000000000000000000000000000000000 with anvil_setCode. Like Arc, every mint and
/// transfer() logs at the system emitter (18 decimals) and then here (6 decimals).
/// Balances are its own (Anvil's native coin stays ETH). Never deploy this anywhere real.
contract ArcMockUsdc {
    IArcSystemEmitter constant EMITTER =
        IArcSystemEmitter(0xffffFFFfFFffffffffffffffFfFFFfffFFFfFFfE);

    mapping(address => uint256) public balanceOf;
    uint8 public constant decimals = 6;

    event Transfer(address indexed from, address indexed to, uint256 value);

    function mint(address to, uint256 value) external {
        balanceOf[to] += value;
        EMITTER.emitTransfer(address(0), to, value * 1e12);
        emit Transfer(address(0), to, value);
    }

    function transfer(address to, uint256 value) external returns (bool) {
        balanceOf[msg.sender] -= value;
        balanceOf[to] += value;
        EMITTER.emitTransfer(msg.sender, to, value * 1e12);
        emit Transfer(msg.sender, to, value);
        return true;
    }
}
