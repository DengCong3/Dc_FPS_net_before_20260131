// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "MyCylinderCharacter.generated.h"

class UInputAction;
class UInputMappingContext;

UCLASS()
class FPS_NET_API AMyCylinderCharacter : public ACharacter
{
    GENERATED_BODY()

public:
    AMyCylinderCharacter();

protected:
    virtual void BeginPlay() override;

public:
    virtual void Tick(float DeltaTime) override;
    virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

private:
    // ===== Enhanced Input =====
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
    UInputMappingContext* DefaultMappingContext;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
    UInputAction* MoveAction;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta = (AllowPrivateAccess = "true"))
    UInputAction* LookAction;

    // ===== Input Functions =====
    void Move(const FInputActionValue& Value);
    void Look(const FInputActionValue& Value);
};