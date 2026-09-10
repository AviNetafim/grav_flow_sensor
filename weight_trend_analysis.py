import csv
from pathlib import Path

AVG_SIZE = 25
STDEV_SIZE = 100
STDEV_MIN = 8 
CONFIG = {
	"input_csv": "weight_20.csv",
	"output_csv": "weight_20_analysis.csv",
}

OUTPUT_HEADER = [
	"cnt",
	"sample_inx",
	"weight_sample",
	"cnt_x_wgt_sum",
	"cnt_sum",
	"wgt_sum",
	"cnt_sqr_sum",
	"cnt_sum * cnt_sum",
	"numerator",
	"denominator",
	"slope",
	"slope_avg",
	"stdev",
]

def load_weight_samples(file_name: str) -> list[int]:
		"""Load integer weight samples, expressed in 10 mg units, from a CSV file.

		Args:
				file_name: Path to the CSV file without the ``.csv`` extension.

		Returns:
				The weight samples as a list of integers.
		"""
		csv_path = Path(file_name).with_suffix(".csv")
		weight_samples: list[int] = []

		with csv_path.open("r", newline="", encoding="utf-8-sig") as csv_file:
				reader = csv.reader(csv_file)
				for row_number, row in enumerate(reader, start=1):
						if len(row) != 1 or not row[0].strip():
								raise ValueError(
										f"Row {row_number} must contain exactly one weight sample"
								)
						try:
								weight_samples.append(int(row[0]))
						except ValueError as error:
								raise ValueError(
										f"Invalid weight sample at row {row_number}: {row[0]!r}"
								) from error

		return weight_samples


def main() -> None:
		weight_samples = load_weight_samples(CONFIG["input_csv"])
		first_sample_index = next(
			(index for index, value in enumerate(weight_samples) if value >= 50),
			None,
		)
		if first_sample_index is None:
			print("No weight sample equals or exceeds 50")
		else:
			first_sample = weight_samples[first_sample_index]
			print(
				f"First weight sample >= 50: {first_sample} "
				f"(index {first_sample_index})"
			)
		print(weight_samples)
		if first_sample_index is None:
			return

		cnt_x_wgt_sum = 0
		cnt_sum = 0
		wgt_sum = 0
		cnt_sqr_sum = 0
		cnt = 1
		slope_avg_buff = [0] * AVG_SIZE
		slope_buff_pntr = 0
		slope_deviation_buff = [0] * STDEV_SIZE
			
		deviation_buffer_pntr = 0 
		output_rows = []
		for sample_inx  in range(first_sample_index,len(weight_samples)):
			cnt_x_wgt_sum += cnt * weight_samples[sample_inx]
			cnt_sum += cnt
			wgt_sum += weight_samples[sample_inx]
			cnt_sqr_sum += cnt * cnt
			numerator = cnt * cnt_x_wgt_sum - cnt_sum * wgt_sum
			denominator =  cnt * cnt_sqr_sum - cnt_sum * cnt_sum
			if denominator == 0:
				slope = 0
			else:	
				slope = int(360 *numerator / denominator)

			# calculate moving average of slope over the last 25 samples

			slope_avg_buff[slope_buff_pntr] = slope 
			slope_buff_pntr += 1
			slope_avg = int(sum(slope_avg_buff) / AVG_SIZE)
			if slope_buff_pntr >= AVG_SIZE:
				slope_buff_pntr = 0

			# calculate slope standard deviation 

			slope_deviation = 10*(slope - slope_avg)
			if slope_deviation < 0:
				slope_deviation = -slope_deviation
			slope_deviation_buff[deviation_buffer_pntr] = slope_deviation
			deviation_buffer_pntr +=1 
			stdev = int(sum(slope_deviation_buff) / STDEV_SIZE)
			if deviation_buffer_pntr >= STDEV_SIZE:
				deviation_buffer_pntr = 0

			if cnt > STDEV_SIZE:
				if stdev < STDEV_MIN:
					print(f"stable flow: {slope_avg} after {sample_inx} ")
					break

			output_rows.append([
				cnt,
				sample_inx,
				weight_samples[sample_inx],
				cnt_x_wgt_sum,
				cnt_sum,
				wgt_sum,
				cnt_sqr_sum,
				cnt_sum * cnt_sum,
				numerator,
				denominator,
				slope,
				slope_avg,
				stdev,
			])

			print(f"cnt: {cnt},cnt_x_wgt_sum: {cnt_x_wgt_sum} cnt_sum: {cnt_sum}, wgt_sum: {wgt_sum}" + 
				f" cnt_sqr_sum: {cnt_sqr_sum}, sqr_cnt_sum:{cnt_sum * cnt_sum}, numerator: {numerator}" + 
				f",denominator: {denominator}, slope: {slope} slope_avg: {slope_avg},stdev: {stdev}")
			cnt += 1

		output_path = Path(CONFIG["output_csv"])
		with output_path.open("w", newline="", encoding="utf-8") as output_file:
			writer = csv.writer(output_file)
			writer.writerow(OUTPUT_HEADER)
			writer.writerows(output_rows)

		print(f"Analysis written to {output_path}")

if __name__ == "__main__":
		main()
