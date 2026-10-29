// SPDX-License-Identifier: MIT
pragma solidity 0.8.24;

/// Minimal 6-decimal ERC-20 stand-in for USDC/USDT in local integration tests.
/// Anyone can mint. Never deploy this anywhere real.
contract MockToken {
    mapping(address => uint256) public balanceOf;
    uint8 public constant decimals = 6;

    event Transfer(address indexed from, address indexed to, uint256 value);

    function mint(address to, uint256 value) external {
        balanceOf[to] += value;
        emit Transfer(address(0), to, value);
    }

    function transfer(address to, uint256 value) external returns (bool) {
        balanceOf[msg.sender] -= value;
        balanceOf[to] += value;
        emit Transfer(msg.sender, to, value);
        return true;
    }
}
