import http from 'k6/http';
import { check } from 'k6';

export const options = {
  scenarios: {
    ramp: {
      executor: 'ramping-vus',
      stages: [
        { duration: '10s', target: 100 },
        { duration: '20s', target: 500 },
        { duration: '20s', target: 1000 },
        { duration: '10s', target: 0 },
      ],
      gracefulRampDown: '5s',
      gracefulStop: '10s',
    },
  },
};

const payload = JSON.stringify({
  id: 'tx-stress-1000',
  transaction: { amount: 9505.97, installments: 10, requested_at: '2026-03-14T05:15:12Z' },
  customer: { avg_amount: 81.28, tx_count_24h: 20, known_merchants: ['MERC-008', 'MERC-007', 'MERC-005'] },
  merchant: { id: 'MERC-068', mcc: '7802', avg_amount: 54.86 },
  terminal: { is_online: false, card_present: true, km_from_home: 952.27 },
  last_transaction: null,
});

export default function () {
  const res = http.post('http://localhost:9999/fraud-score', payload, {
    headers: { 'Content-Type': 'application/json' },
    timeout: '10s',
  });
  check(res, { 'status is 200': (r) => r.status === 200 });
}
