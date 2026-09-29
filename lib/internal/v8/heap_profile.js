'use strict';

const {
  ArrayPrototypeIncludes,
  ArrayPrototypeSlice,
} = primordials;

const {
  codes: {
    ERR_INVALID_ARG_VALUE,
  },
} = require('internal/errors');
const { kEmptyObject } = require('internal/util');
const {
  validateArray,
  validateBoolean,
  validateInteger,
  validateInt32,
  validateObject,
  validateString,
} = require('internal/validators');

const { byteLengthUtf8 } = internalBinding('buffer');

const {
  kSamplingNoFlags,
  kSamplingForceGC,
  kSamplingIncludeObjectsCollectedByMajorGC,
  kSamplingIncludeObjectsCollectedByMinorGC,
} = internalBinding('v8');

function normalizeHeapProfileOptions(options = kEmptyObject) {
  validateObject(options, 'options');
  const {
    sampleInterval = 512 * 1024,
    stackDepth = 16,
    forceGC = false,
    includeObjectsCollectedByMajorGC = false,
    includeObjectsCollectedByMinorGC = false,
    labels = false,
    groupBy,
    maxGroups,
  } = options;

  validateInteger(sampleInterval, 'options.sampleInterval', 1);
  validateInt32(stackDepth, 'options.stackDepth', 0);
  validateBoolean(forceGC, 'options.forceGC');
  validateBoolean(includeObjectsCollectedByMajorGC,
                  'options.includeObjectsCollectedByMajorGC');
  validateBoolean(includeObjectsCollectedByMinorGC,
                  'options.includeObjectsCollectedByMinorGC');
  validateBoolean(labels, 'options.labels');
  if (groupBy !== undefined) {
    validateGroupBy(groupBy);
    if (!labels) {
      throw new ERR_INVALID_ARG_VALUE('options.groupBy', groupBy,
                                      'requires options.labels to be true');
    }
    if (maxGroups !== undefined) {
      validateInteger(maxGroups, 'options.maxGroups', 1, 65536);
    }
  } else if (maxGroups !== undefined) {
    throw new ERR_INVALID_ARG_VALUE('options.maxGroups', maxGroups,
                                    'requires options.groupBy');
  }

  let flags = kSamplingNoFlags;
  if (forceGC) flags |= kSamplingForceGC;
  if (includeObjectsCollectedByMajorGC) {
    flags |= kSamplingIncludeObjectsCollectedByMajorGC;
  }
  if (includeObjectsCollectedByMinorGC) {
    flags |= kSamplingIncludeObjectsCollectedByMinorGC;
  }

  return {
    sampleInterval,
    stackDepth,
    flags,
    labels,
    groupBy: groupBy === undefined ? undefined : ArrayPrototypeSlice(groupBy),
    maxGroups: maxGroups ?? 256,
  };
}

function validateGroupBy(groupBy) {
  validateArray(groupBy, 'options.groupBy', 1);
  if (groupBy.length > 8) {
    throw new ERR_INVALID_ARG_VALUE('options.groupBy', groupBy,
                                    'must have at most 8 keys');
  }
  for (let i = 0; i < groupBy.length; i++) {
    const key = groupBy[i];
    const name = `options.groupBy[${i}]`;
    validateString(key, name);
    if (key === '' || byteLengthUtf8(key) > 255) {
      throw new ERR_INVALID_ARG_VALUE(name, key,
                                      'must be 1 to 255 UTF-8 bytes long');
    }
    if (ArrayPrototypeIncludes(groupBy, key, i + 1)) {
      throw new ERR_INVALID_ARG_VALUE(name, key, 'must be unique');
    }
  }
}

module.exports = {
  normalizeHeapProfileOptions,
};
