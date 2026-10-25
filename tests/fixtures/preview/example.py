from dataclasses import dataclass


@dataclass
class Greeting:
    name: str

    def message(self) -> str:
        # 中文注释也支持显示。
        return f"你好，{self.name}！"


if __name__ == "__main__":
    print(Greeting("Umeko").message())
