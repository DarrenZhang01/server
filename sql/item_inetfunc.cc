/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mariadb.h"
#include "item_inetfunc.h"

#include "my_net.h"

///////////////////////////////////////////////////////////////////////////

static const int IN_ADDR_SIZE= sizeof (in_addr);
static const int IN6_ADDR_SIZE= sizeof (in6_addr);
static const int IN6_ADDR_NUM_WORDS= IN6_ADDR_SIZE / 2;

static const char HEX_DIGITS[]= "0123456789abcdef";

///////////////////////////////////////////////////////////////////////////

longlong Item_func_inet_aton::val_int()
{
  DBUG_ASSERT(fixed);

  uint byte_result= 0;
  ulonglong result= 0;                    // We are ready for 64 bit addresses
  const char *p,* end;
  char c= '.'; // we mark c to indicate invalid IP in case length is 0
  int dot_count= 0;

  StringBuffer<36> tmp;
  String *s= args[0]->val_str_ascii(&tmp);

  if (!s)       // If null value
    goto err;

  null_value= 0;

  end= (p = s->ptr()) + s->length();
  while (p < end)
  {
    c= *p++;
    int digit= (int) (c - '0');
    if (digit >= 0 && digit <= 9)
    {
      if ((byte_result= byte_result * 10 + digit) > 255)
        goto err;                               // Wrong address
    }
    else if (c == '.')
    {
      dot_count++;
      result= (result << 8) + (ulonglong) byte_result;
      byte_result= 0;
    }
    else
      goto err;                                 // Invalid character
  }
  if (c != '.')                                 // IP number can't end on '.'
  {
    /*
      Attempt to support short forms of IP-addresses. It's however pretty
      basic one comparing to the BSD support.
      Examples:
        127     -> 0.0.0.127
        127.255 -> 127.0.0.255
        127.256 -> NULL (should have been 127.0.1.0)
        127.2.1 -> 127.2.0.1
    */
    switch (dot_count) {
    case 1: result<<= 8; /* Fall through */
    case 2: result<<= 8; /* Fall through */
    }
    return (result << 8) + (ulonglong) byte_result;
  }

err:
  null_value=1;
  return 0;
}

///////////////////////////////////////////////////////////////////////////

String* Item_func_inet_ntoa::val_str(String* str)
{
  DBUG_ASSERT(fixed);

  ulonglong n= (ulonglong) args[0]->val_int();

  /*
    We do not know if args[0] is NULL until we have called
    some val function on it if args[0] is not a constant!

    Also return null if n > 255.255.255.255
  */
  if ((null_value= (args[0]->null_value || n > 0xffffffff)))
    return 0;                                   // Null value

  str->set_charset(collation.collation);
  str->length(0);

  uchar buf[8];
  int4store(buf, n);

  /* Now we can assume little endian. */

  char num[4];
  num[3]= '.';

  for (uchar *p= buf + 4; p-- > buf;)
  {
    uint c= *p;
    uint n1, n2;                                // Try to avoid divisions
    n1= c / 100;                                // 100 digits
    c-= n1 * 100;
    n2= c / 10;                                 // 10 digits
    c-= n2 * 10;                                // last digit
    num[0]= (char) n1 + '0';
    num[1]= (char) n2 + '0';
    num[2]= (char) c + '0';
    uint length= (n1 ? 4 : n2 ? 3 : 2);         // Remove pre-zero
    uint dot_length= (p <= buf) ? 1 : 0;
    (void) str->append(num + 4 - length, length - dot_length,
                       &my_charset_latin1);
  }

  return str;
}

///////////////////////////////////////////////////////////////////////////

/**
  Check the function argument, handle errors properly.

  @return The function value.
*/

longlong Item_func_inet_bool_base::val_int()
{
  DBUG_ASSERT(fixed);

  // String argument expected
  if (unlikely(args[0]->result_type() != STRING_RESULT))
    return 0;

  String buffer;
  String *arg_str= args[0]->val_str(&buffer);

  if (unlikely(!arg_str)) // Out-of memory happened. error has been reported.
    return 0;   // Or: the underlying field is NULL

  return calc_value(arg_str) ? 1 : 0;
}


///////////////////////////////////////////////////////////////////////////

/**
  Tries to convert given string to binary IPv4-address representation.
  This is a portable alternative to inet_pton(AF_INET).

  @param      str          String to convert.
  @param      str_len      String length.
  @param[out] ipv4_address Buffer to store IPv4-address.

  @return Completion status.
  @retval false Given string does not represent an IPv4-address.
  @retval true  The string has been converted sucessfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

static bool str_to_ipv4(const char *str, size_t str_length, in_addr *ipv4_address)
{
  if (str_length < 7)
  {
    DBUG_PRINT("error", ("str_to_ipv4(%.*s): "
                         "invalid IPv4 address: too short.",
                         (int) str_length, str));
    return false;
  }

  if (str_length > 15)
  {
    DBUG_PRINT("error", ("str_to_ipv4(%.*s): "
                         "invalid IPv4 address: too long.",
                         (int) str_length, str));
    return false;
  }

  unsigned char *ipv4_bytes= (unsigned char *) ipv4_address;
  const char *p= str;
  int byte_value= 0;
  int chars_in_group= 0;
  int dot_count= 0;
  char c= 0;

  while (((p - str) < (int)str_length) && *p)
  {
    c= *p++;

    if (my_isdigit(&my_charset_latin1, c))
    {
      ++chars_in_group;

      if (chars_in_group > 3)
      {
        DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                             "too many characters in a group.",
                             (int) str_length, str));
        return false;
      }

      byte_value= byte_value * 10 + (c - '0');

      if (byte_value > 255)
      {
        DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                             "invalid byte value.",
                             (int) str_length, str));
        return false;
      }
    }
    else if (c == '.')
    {
      if (chars_in_group == 0)
      {
        DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                             "too few characters in a group.",
                             (int) str_length, str));
        return false;
      }

      ipv4_bytes[dot_count]= (unsigned char) byte_value;

      ++dot_count;
      byte_value= 0;
      chars_in_group= 0;

      if (dot_count > 3)
      {
        DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                             "too many dots.", (int) str_length, str));
        return false;
      }
    }
    else
    {
      DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                           "invalid character at pos %d.",
                           (int) str_length, str, (int) (p - str)));
      return false;
    }
  }

  if (c == '.')
  {
    DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                         "ending at '.'.", (int) str_length, str));
    return false;
  }

  if (dot_count != 3)
  {
    DBUG_PRINT("error", ("str_to_ipv4(%.*s): invalid IPv4 address: "
                         "too few groups.",
                         (int) str_length, str));
    return false;
  }

  ipv4_bytes[3]= (unsigned char) byte_value;

  DBUG_PRINT("info", ("str_to_ipv4(%.*s): valid IPv4 address: %d.%d.%d.%d",
                      (int) str_length, str,
                      ipv4_bytes[0], ipv4_bytes[1],
                      ipv4_bytes[2], ipv4_bytes[3]));
  return true;
}

///////////////////////////////////////////////////////////////////////////

/**
  Tries to convert given string to binary IPv6-address representation.
  This is a portable alternative to inet_pton(AF_INET6).

  @param      str          String to convert.
  @param      str_len      String length.
  @param[out] ipv6_address Buffer to store IPv6-address.

  @return Completion status.
  @retval false Given string does not represent an IPv6-address.
  @retval true  The string has been converted sucessfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

static bool str_to_ipv6(const char *str, int str_length, in6_addr *ipv6_address)
{
  if (str_length < 2)
  {
    DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: too short.",
                         str_length, str));
    return false;
  }

  if (str_length > 8 * 4 + 7)
  {
    DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: too long.",
                         str_length, str));
    return false;
  }

  memset(ipv6_address, 0, IN6_ADDR_SIZE);

  const char *p= str;

  if (*p == ':')
  {
    ++p;

    if (*p != ':')
    {
      DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                           "can not start with ':x'.", str_length, str));
      return false;
    }
  }

  char *ipv6_bytes= (char *) ipv6_address;
  char *ipv6_bytes_end= ipv6_bytes + IN6_ADDR_SIZE;
  char *dst= ipv6_bytes;
  char *gap_ptr= NULL;
  const char *group_start_ptr= p;
  int chars_in_group= 0;
  int group_value= 0;

  while (((p - str) < str_length) && *p)
  {
    char c= *p++;

    if (c == ':')
    {
      group_start_ptr= p;

      if (!chars_in_group)
      {
        if (gap_ptr)
        {
          DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                               "too many gaps(::).", str_length, str));
          return false;
        }

        gap_ptr= dst;
        continue;
      }

      if (!*p || ((p - str) >= str_length))
      {
        DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                             "ending at ':'.", str_length, str));
        return false;
      }

      if (dst + 2 > ipv6_bytes_end)
      {
        DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                             "too many groups (1).", str_length, str));
        return false;
      }

      dst[0]= (unsigned char) (group_value >> 8) & 0xff;
      dst[1]= (unsigned char) group_value & 0xff;
      dst += 2;

      chars_in_group= 0;
      group_value= 0;
    }
    else if (c == '.')
    {
      if (dst + IN_ADDR_SIZE > ipv6_bytes_end)
      {
        DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                             "unexpected IPv4-part.", str_length, str));
        return false;
      }

      if (!str_to_ipv4(group_start_ptr,
                       str + str_length - group_start_ptr,
                       (in_addr *) dst))
      {
        DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                             "invalid IPv4-part.", str_length, str));
        return false;
      }

      dst += IN_ADDR_SIZE;
      chars_in_group= 0;

      break;
    }
    else
    {
      const char *hdp= strchr(HEX_DIGITS, my_tolower(&my_charset_latin1, c));

      if (!hdp)
      {
        DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                             "invalid character at pos %d.",
                             str_length, str, (int) (p - str)));
        return false;
      }

      if (chars_in_group >= 4)
      {
        DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                             "too many digits in group.",
                             str_length, str));
        return false;
      }

      group_value <<= 4;
      group_value |= hdp - HEX_DIGITS;

      DBUG_ASSERT(group_value <= 0xffff);

      ++chars_in_group;
    }
  }

  if (chars_in_group > 0)
  {
    if (dst + 2 > ipv6_bytes_end)
    {
      DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                           "too many groups (2).", str_length, str));
      return false;
    }

    dst[0]= (unsigned char) (group_value >> 8) & 0xff;
    dst[1]= (unsigned char) group_value & 0xff;
    dst += 2;
  }

  if (gap_ptr)
  {
    if (dst == ipv6_bytes_end)
    {
      DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                           "no room for a gap (::).", str_length, str));
      return false;
    }

    int bytes_to_move= (int)(dst - gap_ptr);

    for (int i= 1; i <= bytes_to_move; ++i)
    {
      ipv6_bytes_end[-i]= gap_ptr[bytes_to_move - i];
      gap_ptr[bytes_to_move - i]= 0;
    }

    dst= ipv6_bytes_end;
  }

  if (dst < ipv6_bytes_end)
  {
    DBUG_PRINT("error", ("str_to_ipv6(%.*s): invalid IPv6 address: "
                         "too few groups.", str_length, str));
    return false;
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////

/**
  Converts IPv4-binary-address to a string. This function is a portable
  alternative to inet_ntop(AF_INET).

  @param[in] ipv4 IPv4-address data (byte array)
  @param[out] str A buffer to store string representation of IPv4-address.
                  It must be at least of INET_ADDRSTRLEN.

  @note The problem with inet_ntop() is that it is available starting from
  Windows Vista, but the minimum supported version is Windows 2000.
*/

static void ipv4_to_str(const in_addr *ipv4, char *str)
{
  const unsigned char *ipv4_bytes= (const unsigned char *) ipv4;

  sprintf(str, "%d.%d.%d.%d",
          ipv4_bytes[0], ipv4_bytes[1], ipv4_bytes[2], ipv4_bytes[3]);
}
///////////////////////////////////////////////////////////////////////////

/**
  Converts IPv6-binary-address to a string. This function is a portable
  alternative to inet_ntop(AF_INET6).

  @param[in] ipv6 IPv6-address data (byte array)
  @param[out] str A buffer to store string representation of IPv6-address.
                  It must be at least of INET6_ADDRSTRLEN.

  @note The problem with inet_ntop() is that it is available starting from
  Windows Vista, but out the minimum supported version is Windows 2000.
*/

static void ipv6_to_str(const in6_addr *ipv6, char *str)
{
  struct Region
  {
    int pos;
    int length;
  };

  const unsigned char *ipv6_bytes= (const unsigned char *) ipv6;

  // 1. Translate IPv6-address bytes to words.
  // We can't just cast to short, because it's not guaranteed
  // that sizeof (short) == 2. So, we have to make a copy.

  uint16 ipv6_words[IN6_ADDR_NUM_WORDS];

  for (int i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    ipv6_words[i]= (ipv6_bytes[2 * i] << 8) + ipv6_bytes[2 * i + 1];

  // 2. Find "the gap" -- longest sequence of zeros in IPv6-address.

  Region gap= { -1, -1 };

  {
    Region rg= { -1, -1 };

    for (int i = 0; i < IN6_ADDR_NUM_WORDS; ++i)
    {
      if (ipv6_words[i] != 0)
      {
        if (rg.pos >= 0)
        {
          if (rg.length > gap.length)
            gap= rg;

          rg.pos= -1;
          rg.length= -1;
        }
      }
      else
      {
        if (rg.pos >= 0)
        {
          ++rg.length;
        }
        else
        {
          rg.pos= i;
          rg.length= 1;
        }
      }
    }

    if (rg.pos >= 0)
    {
      if (rg.length > gap.length)
        gap= rg;
    }
  }

  // 3. Convert binary data to string.

  char *p= str;

  for (int i = 0; i < IN6_ADDR_NUM_WORDS; ++i)
  {
    if (i == gap.pos)
    {
      // We're at the gap position. We should put trailing ':' and jump to
      // the end of the gap.

      if (i == 0)
      {
        // The gap starts from the beginning of the data -- leading ':'
        // should be put additionally.

        *p= ':';
        ++p;
      }

      *p= ':';
      ++p;

      i += gap.length - 1;
    }
    else if (i == 6 && gap.pos == 0 &&
             (gap.length == 6 ||                           // IPv4-compatible
              (gap.length == 5 && ipv6_words[5] == 0xffff) // IPv4-mapped
             ))
    {
      // The data represents either IPv4-compatible or IPv4-mapped address.
      // The IPv6-part (zeros or zeros + ffff) has been already put into
      // the string (str). Now it's time to dump IPv4-part.

      ipv4_to_str((const in_addr *) (ipv6_bytes + 12), p);
      return;
    }
    else
    {
      // Usual IPv6-address-field. Print it out using lower-case
      // hex-letters without leading zeros (recommended IPv6-format).
      //
      // If it is not the last field, append closing ':'.

      p += sprintf(p, "%x", ipv6_words[i]);

      if (i != IN6_ADDR_NUM_WORDS - 1)
      {
        *p= ':';
        ++p;
      }
    }
  }

  *p= 0;
}

///////////////////////////////////////////////////////////////////////////

String *Item_func_inet6_aton::val_str(String *buffer)
{
  DBUG_ASSERT(fixed);

  // String argument expected
  if (unlikely(args[0]->result_type() != STRING_RESULT))
  {
    null_value= true;
    return NULL;
  }

  Ascii_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  null_value= tmp.is_null() || !calc_value(tmp.string(), buffer);
  return unlikely(null_value) ? NULL : buffer;
}


/**
  Converts IP-address-string to IP-address-data.

  @param       arg    IP-address-string.
  @param [out] buffer Buffer to store IP-address-data.

  @return Completion status.
  @retval false Given string does not represent an IP-address.
  @retval true  The string has been converted sucessfully.
*/

bool Item_func_inet6_aton::calc_value(const String *arg, String *buffer)
{
  // ipv4-string -> varbinary(4)
  // ipv6-string -> varbinary(16)

  in_addr ipv4_address;
  in6_addr ipv6_address;

  if (str_to_ipv4(arg->ptr(), arg->length(), &ipv4_address))
  {
    buffer->length(0);
    buffer->append((char *) &ipv4_address, sizeof (in_addr), &my_charset_bin);

    return true;
  }

  if (str_to_ipv6(arg->ptr(), arg->length(), &ipv6_address))
  {
    buffer->length(0);
    buffer->append((char *) &ipv6_address, sizeof (in6_addr), &my_charset_bin);

    return true;
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////

/**
  Check the function argument, handle errors properly.

  @param [out] buffer Buffer for string operations.

  @return The function value.
*/

String *Item_func_inet6_ntoa::val_str_ascii(String *buffer)
{
  DBUG_ASSERT(fixed);

  // Binary string argument expected
  if (unlikely(args[0]->result_type() != STRING_RESULT ||
               args[0]->collation.collation != &my_charset_bin))
  {
    null_value= true;
    return NULL;
  }

  String_ptr_and_buffer<STRING_BUFFER_USUAL_SIZE> tmp(args[0]);
  null_value= tmp.is_null() || !calc_value(tmp.string(), buffer);
  return unlikely(null_value) ? NULL : buffer;
}


/**
  Converts IP-address-data to IP-address-string.

  @param       arg    IP-address-data.
  @param [out] buffer Buffer to store IP-address-string.

  @return Completion status.
  @retval false The argument does not correspond to IP-address.
  @retval true  The string has been converted sucessfully.
*/

bool Item_func_inet6_ntoa::calc_value(const Binary_string *arg, String *buffer)
{
  if ((int) arg->length() == IN_ADDR_SIZE)
  {
    char str[INET_ADDRSTRLEN];

    ipv4_to_str((const in_addr *) arg->ptr(), str);

    buffer->length(0);
    buffer->append(str, (uint32) strlen(str), &my_charset_latin1);

    return true;
  }
  else if ((int) arg->length() == IN6_ADDR_SIZE)
  {
    char str[INET6_ADDRSTRLEN];

    ipv6_to_str((const in6_addr *) arg->ptr(), str);

    buffer->length(0);
    buffer->append(str, (uint32) strlen(str), &my_charset_latin1);

    return true;
  }

  DBUG_PRINT("info",
             ("INET6_NTOA(): varbinary(4) or varbinary(16) expected."));
  return false;
}

///////////////////////////////////////////////////////////////////////////

/**
  Checks if the passed string represents an IPv4-address.

  @param arg The string to check.

  @return Check status.
  @retval false The passed string does not represent an IPv4-address.
  @retval true  The passed string represents an IPv4-address.
*/

bool Item_func_is_ipv4::calc_value(const String *arg)
{
  in_addr ipv4_address;

  return str_to_ipv4(arg->ptr(), arg->length(), &ipv4_address);
}

///////////////////////////////////////////////////////////////////////////

/**
  Checks if the passed string represents an IPv6-address.

  @param arg The string to check.

  @return Check status.
  @retval false The passed string does not represent an IPv6-address.
  @retval true  The passed string represents an IPv6-address.
*/

bool Item_func_is_ipv6::calc_value(const String *arg)
{
  in6_addr ipv6_address;

  return str_to_ipv6(arg->ptr(), arg->length(), &ipv6_address);
}

///////////////////////////////////////////////////////////////////////////

/**
  Checks if the passed IPv6-address is an IPv4-compat IPv6-address.

  @param arg The IPv6-address to check.

  @return Check status.
  @retval false The passed IPv6-address is not an IPv4-compatible IPv6-address.
  @retval true  The passed IPv6-address is an IPv4-compatible IPv6-address.
*/

bool Item_func_is_ipv4_compat::calc_value(const String *arg)
{
  if ((int) arg->length() != IN6_ADDR_SIZE || arg->charset() != &my_charset_bin)
    return false;

  return IN6_IS_ADDR_V4COMPAT((struct in6_addr *) arg->ptr());
}

///////////////////////////////////////////////////////////////////////////

/**
  Checks if the passed IPv6-address is an IPv4-mapped IPv6-address.

  @param arg The IPv6-address to check.

  @return Check status.
  @retval false The passed IPv6-address is not an IPv4-mapped IPv6-address.
  @retval true  The passed IPv6-address is an IPv4-mapped IPv6-address.
*/

bool Item_func_is_ipv4_mapped::calc_value(const String *arg)
{
  if ((int) arg->length() != IN6_ADDR_SIZE || arg->charset() != &my_charset_bin)
    return false;

  return IN6_IS_ADDR_V4MAPPED((struct in6_addr *) arg->ptr());
}
