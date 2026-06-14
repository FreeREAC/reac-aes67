// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

'use strict';
'require view';
'require rpc';
'require uci';
'require poll';

// Live status: for each configured stream, call its ubus object
// 'reac-aes67.<name>' method 'status' via the rpc module (this LuCI build has
// no standalone 'ubus' resource — ubus is reached through rpc.declare).

// One status call per object name. rpc.declare binds object+method; we pass the
// object name at call time via the 'object' substitution.
var callStatus = rpc.declare({
	object: 'reac-aes67.%s',
	method: 'status',
	expect: { '': {} }
});

function tierLabel(t) {
	return ({
		clean: _('Clean'), crossfade: _('Crossfade'),
		burst_fade: _('Burst fade'), hold_last: _('Hold-last'),
		silence: _('Silence')
	})[t] || (t || '-');
}

// stream names come from UCI (the configured set); each enabled stream's daemon
// registers reac-aes67.<name>. Name defaults to the section name if unset.
function streamNames() {
	var names = [];
	uci.sections('reac-aes67', 'stream', function (s) {
		if (s.enabled === '0') return;
		names.push(s.name || s['.name']);
	});
	return names;
}

return view.extend({
	load: function () {
		return uci.load('reac-aes67');
	},

	pollStatus: function () {
		var names = streamNames();
		return Promise.all(names.map(function (n) {
			return callStatus(n).then(function (r) {
				// ensure the name is present even if the daemon omitted it
				if (r && !r.name) r.name = n;
				return r;
			}).catch(function () {
				return { name: n, running: false };
			});
		}));
	},

	render: function () {
		var self = this;
		var table = E('table', { 'class': 'table cbi-section-table', 'id': 'reac-status' }, [
			E('tr', { 'class': 'tr table-titles' }, [
				E('th', { 'class': 'th' }, _('Stream')),
				E('th', { 'class': 'th' }, _('Iface')),
				E('th', { 'class': 'th' }, _('Rate / ch')),
				E('th', { 'class': 'th' }, _('Multicast')),
				E('th', { 'class': 'th' }, _('Pkts/s')),
				E('th', { 'class': 'th' }, _('Loss')),
				E('th', { 'class': 'th' }, _('PLC')),
				E('th', { 'class': 'th' }, _('RTP seq')),
				E('th', { 'class': 'th' }, _('Uptime'))
			])
		]);

		function fillRows(results) {
			var trs = [];
			(results || []).filter(Boolean).forEach(function (s) {
				var up = s.running !== false;
				trs.push(E('tr', { 'class': 'tr' }, [
					E('td', { 'class': 'td' }, s.name || '-'),
					E('td', { 'class': 'td' }, up ? (s.iface || '-') : E('em', {}, _('not running'))),
					E('td', { 'class': 'td' }, up ? '%d / %d'.format(s.rate || 0, s.channels || 0) : '-'),
					E('td', { 'class': 'td' }, s.mcast || '-'),
					E('td', { 'class': 'td' }, up ? String(s.packets_per_sec != null ? s.packets_per_sec : '-') : '-'),
					E('td', { 'class': 'td' }, up ? String(s.loss_total != null ? s.loss_total : '-') : '-'),
					E('td', { 'class': 'td' }, up ? tierLabel(s.plc_tier) : '-'),
					E('td', { 'class': 'td' }, up ? String(s.rtp_seq != null ? s.rtp_seq : '-') : '-'),
					E('td', { 'class': 'td' }, up ? '%t'.format(s.uptime_s || 0) : '-')
				]));
			});
			if (!trs.length)
				trs.push(E('tr', { 'class': 'tr placeholder' },
					E('td', { 'class': 'td', 'colspan': 9 }, _('No streams configured. Add one under Streams.'))));
			cbi_update_table(table, trs);
		}

		poll.add(function () {
			return self.pollStatus().then(fillRows);
		}, 2);

		return self.pollStatus().then(function (rows) {
			fillRows(rows);
			return E('div', { 'class': 'cbi-map' }, [
				E('h2', {}, _('REAC → AES67 — Live Status')),
				E('div', { 'class': 'cbi-section' }, [ table ])
			]);
		});
	},

	handleSaveApply: null, handleSave: null, handleReset: null
});
