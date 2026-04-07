/*
 * FlexQL: Streaming wire protocol constants.
 */

#ifndef FLEXQL_PROTOCOL_H
#define FLEXQL_PROTOCOL_H

namespace protocol {

inline constexpr char kQueryPrefix[] = "QUERY\n";
inline constexpr char kExit[] = "EXIT";
inline constexpr char kAbort[] = "ABORT";

inline constexpr char kHeaderPrefix[] = "HEADER\n";
inline constexpr char kRowPrefix[] = "ROW\n";
inline constexpr char kErrorPrefix[] = "ERR\n";
inline constexpr char kEnd[] = "END";
inline constexpr char kAborted[] = "ABORTED";

} // namespace protocol

#endif /* FLEXQL_PROTOCOL_H */
