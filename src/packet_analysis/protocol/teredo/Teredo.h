#pragma once

#include "zeek/packet_analysis/Analyzer.h"
#include "zeek/NetVar.h"
#include "zeek/Reporter.h"

namespace zeek::packet_analysis::teredo {

class TeredoAnalyzer final : public packet_analysis::Analyzer {
public:
	TeredoAnalyzer();
	~TeredoAnalyzer() override = default;

	bool AnalyzePacket(size_t len, const uint8_t* data, Packet* packet) override;

	static zeek::packet_analysis::AnalyzerPtr Instantiate()
		{
		return std::make_shared<TeredoAnalyzer>();
		}

	/**
	 * Emits a weird only if the analyzer has previously been able to
	 * decapsulate a Teredo packet in both directions or if *force* param is
	 * set, since otherwise the weirds could happen frequently enough to be less
	 * than helpful.  The *force* param is meant for cases where just one side
	 * has a valid encapsulation and so the weird would be informative.
	 */
	void Weird(const char* name, bool force = false) const
		{
		// TODO
		if ( /*ProtocolConfirmed() ||*/ force )
			reporter->Weird(conn, name, "", GetAnalyzerName());
		}

	/**
	 * If the delayed confirmation option is set, then a valid encapsulation
	 * seen from both end points is required before confirming.
	 */
	void Confirm()
		{
		// TODO
		// if ( ! BifConst::Tunnel::delay_teredo_confirmation ||
		//      ( valid_orig && valid_resp ) )
		// 	ProtocolConfirmation();
		}

	bool DetectProtocol(size_t len, const uint8_t* data, Packet* packet) override;

protected:
	bool valid_orig = false;
	bool valid_resp = false;
	Connection* conn = nullptr;
};

namespace detail {

class TeredoEncapsulation {
public:
	explicit TeredoEncapsulation(const TeredoAnalyzer* ta) : analyzer(ta) { }

	/**
	 * Returns whether input data parsed as a valid Teredo encapsulation type.
	 * If it was valid, the len argument is decremented appropriately.
	 */
	bool Parse(const u_char* data, size_t& len)
		{ return DoParse(data, len, false, false); }

	const u_char* InnerIP() const
		{ return inner_ip; }

	const u_char* OriginIndication() const
		{ return origin_indication; }

	const u_char* Authentication() const
		{ return auth; }

	RecordValPtr BuildVal(const std::shared_ptr<IP_Hdr>& inner) const;

protected:
	bool DoParse(const u_char* data, size_t& len, bool found_orig, bool found_au);

	void Weird(const char* name) const	{ analyzer->Weird(name); }

	const u_char* inner_ip = nullptr;
	const u_char* origin_indication = nullptr;
	const u_char* auth = nullptr;
	const TeredoAnalyzer* analyzer = nullptr;
};

} // namespace detail

} // namespace zeek::packet_analysis::teredo
