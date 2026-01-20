/****************************************************************************
** Copyright (c) 2001-2014
**
** This file is part of the QuickFIX FIX Engine
**
** This file may be distributed under the terms of the quickfixengine.org
** license as defined by quickfixengine.org and appearing in the file
** LICENSE included in the packaging of this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.quickfixengine.org/LICENSE for licensing information.
**
** Contact ask@quickfixengine.org if any conditions of this licensing are
** not clear to you.
**
****************************************************************************/

#ifdef _MSC_VER
#include "stdafx.h"
#else
#include "config.h"
#endif

#include "FieldConvertors.h"
#include "Parser.h"
#include "Utility.h"
#include <algorithm>
#include <cstring>

namespace FIX {

// Helper: find a pattern in buffer using memchr for speed
// Returns position relative to buffer start, or npos if not found
static inline std::string::size_type findPattern(
    const char* data, size_t dataLen, size_t startPos,
    const char* pattern, size_t patternLen) {
  if (startPos + patternLen > dataLen) {
    return std::string::npos;
  }

  const char* searchStart = data + startPos;
  size_t searchLen = dataLen - startPos;
  const char firstChar = pattern[0];

  while (searchLen >= patternLen) {
    // Use memchr to find first character quickly
    const char* found = static_cast<const char*>(
        memchr(searchStart, firstChar, searchLen));
    if (!found) {
      return std::string::npos;
    }

    size_t remaining = searchLen - (found - searchStart);
    if (remaining < patternLen) {
      return std::string::npos;
    }

    // Check if full pattern matches
    if (memcmp(found, pattern, patternLen) == 0) {
      return found - data;
    }

    // Move past this character and continue
    searchStart = found + 1;
    searchLen = remaining - 1;
  }
  return std::string::npos;
}

// Helper: find single character using memchr
static inline std::string::size_type findChar(
    const char* data, size_t dataLen, size_t startPos, char c) {
  if (startPos >= dataLen) {
    return std::string::npos;
  }
  const char* found = static_cast<const char*>(
      memchr(data + startPos, c, dataLen - startPos));
  return found ? (found - data) : std::string::npos;
}

// Helper: parse integer directly without creating temporary string
static inline bool parseIntFast(const char* start, const char* end, int& result) {
  if (start >= end) {
    return false;
  }

  bool negative = false;
  if (*start == '-') {
    negative = true;
    ++start;
    if (start >= end) {
      return false;
    }
  }

  int value = 0;
  while (start < end) {
    char c = *start++;
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }

  result = negative ? -value : value;
  return true;
}

bool Parser::extractLength(int &length, std::string::size_type &pos, const std::string &buffer)
    EXCEPT(MessageParseError) {
  const size_t bufLen = buffer.size();
  if (bufLen == 0) {
    return false;
  }

  const char* data = buffer.data();

  // Find "\0019=" pattern
  std::string::size_type startPos = findPattern(data, bufLen, 0, "\0019=", 3);
  if (startPos == std::string::npos) {
    return false;
  }
  startPos += 3;

  // Find terminating SOH
  std::string::size_type endPos = findChar(data, bufLen, startPos, '\001');
  if (endPos == std::string::npos) {
    return false;
  }

  // Parse length directly without creating string
  if (!parseIntFast(data + startPos, data + endPos, length)) {
    throw MessageParseError();
  }

  if (length < 0) {
    throw MessageParseError();
  }

  pos = endPos + 1;
  return true;
}

bool Parser::readFixMessage(std::string &str) EXCEPT(MessageParseError) {
  const size_t bufLen = m_buffer.size();

  // Need at least "8=" to start
  if (bufLen < m_readPos + 2) {
    return false;
  }

  const char* data = m_buffer.data();

  // Find message start "8="
  std::string::size_type msgStart = findPattern(data, bufLen, m_readPos, "8=", 2);
  if (msgStart == std::string::npos) {
    return false;
  }

  // Skip any garbage before the message start
  if (msgStart > m_readPos) {
    m_readPos = msgStart;
  }

  // Create a view of the buffer from current read position
  const char* msgData = data + m_readPos;
  const size_t msgBufLen = bufLen - m_readPos;

  // Find "\0019=" for body length
  std::string::size_type lenTagPos = findPattern(msgData, msgBufLen, 0, "\0019=", 3);
  if (lenTagPos == std::string::npos) {
    return false;
  }
  std::string::size_type lenValueStart = lenTagPos + 3;

  // Find SOH after length value
  std::string::size_type lenValueEnd = findChar(msgData, msgBufLen, lenValueStart, '\001');
  if (lenValueEnd == std::string::npos) {
    return false;
  }

  // Parse body length
  int bodyLength = 0;
  if (!parseIntFast(msgData + lenValueStart, msgData + lenValueEnd, bodyLength)) {
    // Bad length - clear buffer and throw
    m_buffer.clear();
    m_readPos = 0;
    throw MessageParseError();
  }

  if (bodyLength < 0) {
    m_buffer.clear();
    m_readPos = 0;
    throw MessageParseError();
  }

  // Position after body length field
  std::string::size_type bodyStart = lenValueEnd + 1;
  std::string::size_type expectedEnd = bodyStart + bodyLength;

  // Check if we have enough data
  if (msgBufLen < expectedEnd) {
    return false;
  }

  // Find checksum field "\00110=" starting near expected end
  std::string::size_type searchStart = (expectedEnd > 1) ? expectedEnd - 1 : 0;
  std::string::size_type checksumPos = findPattern(msgData, msgBufLen, searchStart, "\00110=", 4);
  if (checksumPos == std::string::npos) {
    return false;
  }

  // Find SOH after checksum value
  std::string::size_type checksumEnd = findChar(msgData, msgBufLen, checksumPos + 4, '\001');
  if (checksumEnd == std::string::npos) {
    return false;
  }

  // Message length is up to and including final SOH
  std::string::size_type msgLen = checksumEnd + 1;

  // Extract the complete message
  str.assign(msgData, msgLen);

  // Advance read position
  m_readPos += msgLen;

  // Compact buffer if needed
  compactBufferIfNeeded();

  return true;
}
} // namespace FIX
