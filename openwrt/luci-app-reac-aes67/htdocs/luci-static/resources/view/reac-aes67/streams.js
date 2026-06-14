// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

'use strict';
'require view';
'require form';
'require uci';

// Config view: edit the global enable + up to 3 REAC->AES67 streams.
// Bound 1:1 to /etc/config/reac-aes67; Save&Apply writes UCI and reloads the
// service (handled by LuCI's apply + the procd reload trigger).
return view.extend({
	render: function () {
		let m, s, o;

		m = new form.Map('reac-aes67', _('REAC → AES67 Bridge'),
			_('Decode Roland REAC (EtherType 0x8819) on this router and re-emit ' +
			  'each stream as AES67 (RTP L24 multicast) for PipeWire/AES67 receivers. ' +
			  'Rate is NOT auto-detected: set it to match the REAC clock master ' +
			  '(40 channels at every rate; 96 kHz doubles the packet rate, not the channel count).'));

		s = m.section(form.NamedSection, 'global', 'reac-aes67', _('Service'));
		s.addremove = false;
		o = s.option(form.Flag, 'enabled', _('Enabled'),
			_('Master on/off for the whole bridge.'));
		o.rmempty = false;

		s = m.section(form.GridSection, 'stream', _('REAC Streams'),
			_('One AES67 multicast stream per REAC zone (up to 3).'));
		s.addremove = true;
		s.anonymous = false;
		s.nodescriptions = true;

		o = s.option(form.Flag, 'enabled', _('On'));
		o.rmempty = false;

		o = s.option(form.Value, 'name', _('Name'));
		o.placeholder = 'REAC-A';

		o = s.option(form.Value, 'iface', _('Capture iface'),
			_('Interface carrying the REAC zone (e.g. lan1/lan2/lan3).'));
		o.placeholder = 'lan1';

		o = s.option(form.ListValue, 'rate', _('Sample rate'));
		o.value('48000', '48 kHz (40 ch)');
		o.value('96000', '96 kHz (40 ch)');
		o.default = '48000';

		o = s.option(form.Value, 'mcast_addr', _('Multicast addr'));
		o.datatype = 'ip4addr';
		o.placeholder = '239.69.0.1';

		o = s.option(form.Value, 'mcast_port', _('Port'));
		o.datatype = 'port';
		o.placeholder = '5004';

		o = s.option(form.Value, 'payload_type', _('RTP PT'));
		o.datatype = 'range(96,127)';
		o.default = '97';
		o.modalonly = true;

		o = s.option(form.Value, 'ssrc', _('SSRC (hex)'));
		o.placeholder = '11223344';
		o.modalonly = true;

		o = s.option(form.Value, 'ttl', _('Multicast TTL'));
		o.datatype = 'range(1,255)';
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Flag, 'sap', _('SAP discovery'),
			_('Announce this stream via SAP (AES67) so receivers auto-discover it.'));
		o.default = '1';
		o.modalonly = true;

		o = s.option(form.Value, 'plc_xfade', _('PLC crossfade (samples)'),
			_('0 = default. Loss-concealment in-splice crossfade width.'));
		o.datatype = 'uinteger';
		o.modalonly = true;

		o = s.option(form.Value, 'plc_fade_pkts', _('PLC burst fade (packets)'),
			_('0 = default (~50 ms). Length of the fade-to-silence on long loss bursts.'));
		o.datatype = 'uinteger';
		o.modalonly = true;

		return m.render();
	}
});
