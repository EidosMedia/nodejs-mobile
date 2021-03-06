'use strict';
const common = require('../common');
const assert = require('assert');
const url = require('url');

// https://github.com/joyent/node/issues/568
const errMessage = /^TypeError: Parameter "url" must be a string, not (?:undefined|boolean|number|object|function|symbol)$/;
[
  undefined,
  null,
  true,
  false,
  0.0,
  0,
  [],
  {},
  () => {},
  Symbol('foo')
].forEach((val) => {
  assert.throws(() => { url.parse(val); }, errMessage);
});

assert.throws(
  () => { url.parse('http://%E0%A4%A@fail'); },
  common.engineSpecificMessage({
    v8: /^URIError: URI malformed$/,
    chakracore: /^URIError: The URI to be decoded is not a valid encoding$/
  })
);
