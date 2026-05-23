import http from 'k6/http';
import { check } from 'k6';

export const options = {
  scenarios: {
    ramp: {
      executor: 'ramping-vus',
      stages: [
        { duration: '10s', target: 10 },
        { duration: '20s', target: 50 },
        { duration: '20s', target: 100 },
        { duration: '10s', target: 0 },
      ],
    },
  },
  thresholds: {
    http_req_failed: ['rate<0.01'],
    http_req_duration: ['p(99)<2000'],
  },
};

const url = __ENV.API_URL || 'http://localhost:9999/fraud-score';

const payloads = [
  {
    id: 'tx-1329056812',
    transaction: { amount: 41.12, installments: 2, requested_at: '2026-03-11T18:45:53Z' },
    customer: { avg_amount: 82.24, tx_count_24h: 3, known_merchants: ['MERC-003', 'MERC-016'] },
    merchant: { id: 'MERC-016', mcc: '5411', avg_amount: 60.25 },
    terminal: { is_online: false, card_present: true, km_from_home: 29.23 },
    last_transaction: null,
  },
  {
    id: 'tx-3330991687',
    transaction: { amount: 9505.97, installments: 10, requested_at: '2026-03-14T05:15:12Z' },
    customer: { avg_amount: 81.28, tx_count_24h: 20, known_merchants: ['MERC-008', 'MERC-007', 'MERC-005'] },
    merchant: { id: 'MERC-068', mcc: '7802', avg_amount: 54.86 },
    terminal: { is_online: false, card_present: true, km_from_home: 952.27 },
    last_transaction: null,
  },
];

export default function () {
  const payload = payloads[__ITER % payloads.length];
  const res = http.post(url, JSON.stringify(payload), {
    headers: { 'Content-Type': 'application/json' },
  });

  check(res, {
    'status is 200': (r) => r.status === 200,
    'has approved': (r) => r.json('approved') !== undefined,
    'has fraud_score': (r) => r.json('fraud_score') !== undefined,
  });
}
