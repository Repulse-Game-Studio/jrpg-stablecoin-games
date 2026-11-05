#!/usr/bin/env node
"use strict";

process.exitCode = require("../lib/install.js").main(process.argv.slice(2));
